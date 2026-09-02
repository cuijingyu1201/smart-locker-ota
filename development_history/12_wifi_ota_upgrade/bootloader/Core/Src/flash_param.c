#include "flash_param.h"
#include "flash_if.h"   /* FLASH_ErasePages / FLASH_WriteBuf / FLASH_ReadWord */
#include <stdio.h>
#include <string.h>

/* ================================================================
 *   CRC32 lookup table (IEEE 802.3, polynomial 0xEDB88320)
 *   Pros: only 256 entries, computation guaranteed correct
 *   Cons: CRC32_InitTable() takes ~50us at boot, table uses 1KB RAM
 * ================================================================ */
static uint32_t crc32_table[256];
static flash_param_t flash_param_save_buf;

/* Build the 256-entry table from polynomial 0xEDB88320 (call once at boot) */
void CRC32_InitTable(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t bit = 0; bit < 8; bit++) {
            /* if LSB is 1, shift right and XOR with polynomial */
            crc = (crc & 1u) ? ((crc >> 1) ^ CRC32_POLYNOMIAL) : (crc >> 1);
        }
        crc32_table[i] = crc;
    }
}

/* Standard CRC32 of a memory block: init 0xFFFFFFFF, finalize XOR */
uint32_t CRC32_Calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* CRC32 of the entire APP area (0x08008000, 464KB) */
uint32_t CRC32_CalcAppFlash(void)
{
    return CRC32_Calc((const uint8_t *)APP_FLASH_START, APP_FLASH_SIZE);
}

/* ================================================================
 *   Internal: compute struct CRC32 (excludes magic_header and struct_crc32 fields)
 * ================================================================ */
static uint32_t param_calc_struct_crc(const flash_param_t *p)
{
    /* CRC range: from offset 8 (skip magic_header 4B + struct_crc32 4B)
     *            to offset 0xFF0 (stop before Tail fields, which have their own CRC) */
    const uint8_t *start = (const uint8_t *)p + 8;
    const uint8_t *end   = (const uint8_t *)p + 0xFF0;
    return CRC32_Calc(start, (uint32_t)(end - start));
}

/* ================================================================
 *   FlashParam_Load: read param area from Flash into RAM (p_out)
 *   Returns 0=success (magic+CRC ok), -1=fail (uninitialized or corrupted)
 * ================================================================ */
int FlashParam_Load(flash_param_t *p_out)
{
    if (p_out == NULL) return -1;

    /* 1. Direct memcpy from Flash mapped address to RAM (Flash is memory-mapped) */
    memcpy(p_out, (const void *)PARAM_FLASH_START, sizeof(flash_param_t));

    /* 2. Check magic_header */
    if (p_out->magic_header != MAGIC_PARAM_HEADER) {
        return -1;  /* not initialized (all 0xFF) or corrupted */
    }

    /* 3. Check tail_marker (detect partial write / power loss during Flash write) */
    if (p_out->tail_marker != 0xDEADBEEFUL) {
        return -1;
    }

    /* 4. Verify struct_crc32 (detect field corruption / struct mismatch) */
    uint32_t expected_crc = param_calc_struct_crc(p_out);
    if (expected_crc != p_out->struct_crc32) {
        return -1;  /* CRC mismatch: field corrupted or struct version changed */
    }

    return 0;
}

/* ================================================================
 *   FlashParam_Save: write param area (erase + program, auto CRC)
 * ================================================================ */
int FlashParam_Save(const flash_param_t *p_in)
{
    if (p_in == NULL) return -1;

    /* 1. Build a local copy in RAM, fill in magic, CRC, tail */
    memcpy(&flash_param_save_buf, p_in, sizeof(flash_param_t));
    flash_param_save_buf.magic_header = MAGIC_PARAM_HEADER;
    flash_param_save_buf.tail_marker  = 0xDEADBEEFUL;
    flash_param_save_buf.struct_crc32 = param_calc_struct_crc(&flash_param_save_buf);

    /* 2. Erase the flash page(s) where the param area resides */
    if (FLASH_ErasePages(PARAM_FLASH_START,
                         PARAM_FLASH_SIZE / BL_FLASH_PAGE_SIZE) != HAL_OK) {
        return -1;
    }

    /* 3. Write 4-byte aligned (4096 bytes, aligned to flash word size) */
    if (FLASH_WriteBuf(PARAM_FLASH_START, (uint8_t *)&flash_param_save_buf,
                       sizeof(flash_param_t)) != HAL_OK) {
        return -1;
    }

    return 0;
}

/* ================================================================
 *   FlashParam_SetOtaRequest: write OTA request fields then full-page save
 *   After this, APP calls NVIC_SystemReset() to enter Bootloader
 *
 *   NOTE: flash_param_t is ~4KB, so the work buffer must stay in BSS.
 * ================================================================ */
int FlashParam_SetOtaRequest(uint32_t new_fw_crc32, uint32_t new_fw_size,
                            uint16_t ver_major, uint16_t ver_minor,
                            uint16_t ver_patch, uint16_t build_num)
{
    /* Flash can only write 1->0, not 0->1.
     * OTA fields were previously 0xFF, so a direct write would work,
     * but to be safe we do: read -> modify -> save (erase+write) */
    static flash_param_t p;
    int ret = FlashParam_Load(&p);
    if (ret != 0) {
        /* Param area not initialized: fill with defaults, then write OTA flag */
        memset(&p, 0xFF, sizeof(p));
        p.boot_count = 0;
        p.fw_ver_major = 0;
        p.fw_ver_minor = 0;
        p.fw_ver_patch = 0;
    }
    p.ota_request_magic = MAGIC_OTA_REQUEST;
    p.ota_new_fw_crc32  = new_fw_crc32;
    p.ota_new_fw_size   = new_fw_size;
    p.ota_new_fw_ver_major = ver_major;
    p.ota_new_fw_ver_minor = ver_minor;
    p.ota_new_fw_ver_patch = ver_patch;
    p.ota_new_fw_build_num = build_num;
    return FlashParam_Save(&p);
}

/* ================================================================
 *   FlashParam_ClearOtaRequest: clear OTA flag (called by Bootloader
 *   after detecting the flag, before pulling firmware)
 *
 *   NOTE: must use static! flash_param_t is ~4KB, Bootloader stack is
 *   only 4KB (was 1KB before fix). A local on stack would overflow.
 *   Bootloader is single-threaded bare-metal, static is safe.
 * ================================================================ */
int FlashParam_ClearOtaRequest(void)
{
    static flash_param_t p;
    if (FlashParam_Load(&p) != 0) {
        return 0;   /* param area invalid, nothing to clear */
    }
    p.ota_request_magic = 0x00000000;
    p.ota_new_fw_crc32  = 0xFFFFFFFF;
    p.ota_new_fw_size   = 0xFFFFFFFF;
    p.ota_new_fw_ver_major = 0xFFFFU;
    p.ota_new_fw_ver_minor = 0xFFFFU;
    p.ota_new_fw_ver_patch = 0xFFFFU;
    p.ota_new_fw_build_num = 0xFFFFU;
    return FlashParam_Save(&p);
}

/* ================================================================
 *   FlashParam_Print: debug dump (used during D9 verification)
 * ================================================================ */
void FlashParam_Print(const flash_param_t *p)
{
    if (p == NULL) {
        printf("[PARAM] NULL pointer!\r\n");
        return;
    }
    printf("\r\n==== Flash Param Dump @0x%08X ====\r\n", PARAM_FLASH_START);
    printf("  magic_header    = 0x%08X (%c%c%c%c)\r\n", p->magic_header,
           (p->magic_header >> 0) & 0xFF, (p->magic_header >> 8) & 0xFF,
           (p->magic_header >> 16) & 0xFF, (p->magic_header >> 24) & 0xFF);
    printf("  struct_crc32    = 0x%08X\r\n", p->struct_crc32);
    printf("  struct_version  = %u\r\n", p->struct_version);
    printf("  fw_version      = %u.%u.%u (build %u)\r\n",
           p->fw_ver_major, p->fw_ver_minor, p->fw_ver_patch, p->fw_build_num);
    printf("  fw_size_bytes   = %u (0x%08lX)\r\n", p->fw_size_bytes, p->fw_size_bytes);
    printf("  fw_crc32        = 0x%08X\r\n", p->fw_crc32);
    printf("  ota_req_magic   = 0x%08X (%c%c%c%c)\r\n", p->ota_request_magic,
           (p->ota_request_magic >> 0) & 0xFF, (p->ota_request_magic >> 8) & 0xFF,
           (p->ota_request_magic >> 16) & 0xFF, (p->ota_request_magic >> 24) & 0xFF);
    printf("  ota_new_fw_crc  = 0x%08X\r\n", p->ota_new_fw_crc32);
    printf("  ota_new_fw_size = %u\r\n", p->ota_new_fw_size);
    printf("  ota_new_version = %u.%u.%u.%u\r\n",
           p->ota_new_fw_ver_major, p->ota_new_fw_ver_minor,
           p->ota_new_fw_ver_patch, p->ota_new_fw_build_num);
    printf("  ota_rollback    = %u\r\n", p->ota_rollback_count);
    printf("  boot_count      = %u\r\n", p->boot_count);
    printf("  last_reset_reas = 0x%08X\r\n", p->last_reset_reason);
    printf("  last_ota_result = %u\r\n", p->last_ota_result);
    printf("  device_id       = %.16s\r\n", p->device_id);
    printf("  mqtt_prefix     = %.32s\r\n", p->mqtt_topic_prefix);
    printf("  tail_marker     = 0x%08X\r\n", p->tail_marker);
    printf("  tail_crc32      = 0x%08X\r\n", p->tail_crc32);
    printf("=========================================\r\n\r\n");
}

/* ================================================================
 *   CRC32 streaming interface (for D10 IAP and D12 OTA)
 *   Uses the same crc32_table from this file (one table, one algorithm
 *   shared by flash_param.c and iap.c, no need to duplicate)
 *
 *   Usage:
 *     uint32_t ctx;
 *     CRC32_StreamReset(&ctx);
 *     CRC32_StreamUpdate(&ctx, data1, len1);
 *     CRC32_StreamUpdate(&ctx, data2, len2);
 *     uint32_t result = CRC32_StreamFinalize(&ctx);
 * ================================================================ */
void CRC32_StreamReset(uint32_t *ctx)
{
    if (ctx) *ctx = 0xFFFFFFFFUL;
}

void CRC32_StreamUpdate(uint32_t *ctx, const uint8_t *data, uint32_t len)
{
    if (!ctx || !data) return;
    uint32_t crc = *ctx;
    for (uint32_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    *ctx = crc;
}

uint32_t CRC32_StreamFinalize(uint32_t *ctx)
{
    if (!ctx) return 0xFFFFFFFFUL;
    return (*ctx) ^ 0xFFFFFFFFUL;
}
