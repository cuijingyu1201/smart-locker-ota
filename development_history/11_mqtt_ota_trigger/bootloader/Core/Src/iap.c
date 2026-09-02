#include "iap.h"
#include "ymodem.h"      /* IAP state/packet handling. */
#include "flash_if.h"    /* FLASH_EraseAppArea + FLASH_WriteBuf */
#include "flash_param.h" /* FlashParam_Load/Save/ClearOtaRequest + CRC32_Calc */
#include "flash_partition.h"  /* APP_FLASH_START / APP_FLASH_SIZE / MAGIC_OTA_REQUEST / flash_param_t */
#include <stdio.h>
#include <string.h>

/* IAP state/packet handling. */

/* IAP state/packet handling. */
static uint32_t g_running_crc;

/* IAP state/packet handling. */
static uint32_t g_total_size;

/* IAP state/packet handling. */
static uint32_t g_written;

/* IAP state/packet handling. */
static int      g_flash_write_err;

/* IAP state/packet handling. */
static flash_param_t g_param_buf_iap;


/* IAP state/packet handling. */
static int iap_on_packet(uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (g_flash_write_err) return -1;   /* IAP state/packet handling. */

    /* IAP state/packet handling. */
    CRC32_StreamUpdate(&g_running_crc, data, len);

    /* IAP state/packet handling. */
    uint32_t flash_addr = APP_FLASH_START + offset;

    /* IAP state/packet handling. */
    if (flash_addr + len > APP_FLASH_START + APP_FLASH_SIZE) {
        printf("[IAP] [offset=0x%08X len=%u] Out of APP area! Cancel\r\n",
               (unsigned)offset, len);
        g_flash_write_err = 1;
        return -1;
    }

    HAL_StatusTypeDef st = FLASH_WriteBuf(flash_addr, (uint8_t *)data, len);
    if (st != HAL_OK) {
        printf("[IAP] [offset=0x%08X len=%u] FLASH_WriteBuf FAIL, CANCEL\r\n",
               (unsigned)offset, len);
        g_flash_write_err = 1;
        return -1;
    }

    g_written += len;

    /* IAP state/packet handling. */
    if (offset != 0 && (offset % (1024 * 64)) == 0) {
        printf("[IAP] Write progress: offset=0x%08X / %u KB written\r\n",
               (unsigned)offset, (unsigned)(g_written / 1024));
    }

    return 0;   /* IAP state/packet handling. */
}

/* IAP state/packet handling. */
int IAP_ProcessSerial(void)
{
    int ret;

    printf("\r\n========== [IAP] Serial Ymodem Mode ==========\r\n");

    /* IAP state/packet handling. */
    CRC32_StreamReset(&g_running_crc);    /* IAP state/packet handling. */
    g_total_size      = 0;
    g_written         = 0;
    g_flash_write_err = 0;
	  /* IAP state/packet handling. */
    printf("[IAP] Pre-erasing APP flash (0x%08X, %u bytes, 232 pages)...\r\n",
           APP_FLASH_START, APP_FLASH_SIZE);
    HAL_StatusTypeDef est = FLASH_EraseAppArea();
    if (est != HAL_OK) {
        printf("[IAP] !!! Pre-erase FAIL, abort IAP.\r\n");
        return -1;
    }
    printf("[IAP] Pre-erase OK. Now sending 'C' for Ymodem handshake...\r\n");


    /* IAP state/packet handling. */
    ret = Ymodem_Receive(NULL, APP_FLASH_SIZE, &g_total_size, iap_on_packet);

    if (ret != Y_OK) {
        const YmodemDebugInfo *dbg = Ymodem_GetDebugInfo();
        uint32_t dbg_i;
        printf("[IAP] Ymodem FAIL! code=%d (size=%u bytes, flash_written=%u bytes)\r\n",
               ret, g_total_size, g_written);
        printf("[IAP] Ymodem debug: phase=%lu frame=%lu retry=%lu frame_ret=%ld "
               "hdr=0x%02X seq=%u neg=0x%02X len=%u "
               "crc_rx=0x%04X crc_calc=0x%04X "
               "header_ok=%u hhdr=0x%02X hseq=%u hlen=%u "
               "header_size=%lu parse=%ld hal=%lu uart_err=0x%08lX "
               "rx_state=%lu tx_state=%lu\r\n",
               (unsigned long)dbg->phase,
               (unsigned long)dbg->frame_count,
               (unsigned long)dbg->retry_count,
               (long)dbg->last_frame_ret,
               dbg->last_header,
               dbg->last_seq,
               dbg->last_seq_neg,
               dbg->last_data_len,
               dbg->last_crc_rx,
               dbg->last_crc_calc,
               dbg->header_accepted,
               dbg->header_frame_header,
               dbg->header_seq,
               dbg->header_data_len,
               (unsigned long)dbg->header_file_size,
               (long)dbg->header_parse_result,
               (unsigned long)dbg->last_hal_status,
               (unsigned long)dbg->uart_error,
               (unsigned long)dbg->uart_rx_state,
               (unsigned long)dbg->uart_tx_state);
        printf("[IAP] Ymodem header data (first 16 bytes):");
        for (dbg_i = 0; dbg_i < sizeof(dbg->header_preview); ++dbg_i) {
            printf(" %02X", dbg->header_preview[dbg_i]);
        }
        printf("\r\n");
        printf("[IAP] Ymodem last data (first 16 bytes):");
        for (dbg_i = 0; dbg_i < sizeof(dbg->last_data_preview); ++dbg_i) {
            printf(" %02X", dbg->last_data_preview[dbg_i]);
        }
        printf("\r\n");
        /* IAP state/packet handling. */
        printf("[IAP] APP flash may be corrupted. Hold KEY0 + Reset to retry Serial IAP.\r\n");
        /* IAP state/packet handling. */
        return -1;
    }

    if (g_total_size == 0) {
        printf("[IAP] File size is 0, nothing written.\r\n");
        return -1;
    }

    if (g_flash_write_err) {
        printf("[IAP] Flash write error occurred during transfer, cannot trust firmware.\r\n");
        return -1;
    }

    /* IAP state/packet handling. */

    /* IAP state/packet handling. */
    uint32_t crc_ram = CRC32_StreamFinalize(&g_running_crc);

    /* IAP state/packet handling. */
    printf("[IAP] Verifying: CRC calc from flash (0x%08X, %u bytes)...\r\n",
           APP_FLASH_START, g_total_size);
    uint32_t crc_flash = CRC32_Calc((const uint8_t *)APP_FLASH_START, g_total_size);

    printf("[IAP] CRC double check:\r\n");
    printf("[IAP]   CRC_ram   (stream accum) = 0x%08X\r\n", crc_ram);
    printf("[IAP]   CRC_flash (read-back)   = 0x%08X\r\n", crc_flash);

    if (crc_ram != crc_flash) {
        printf("[IAP] !!! CRC MISMATCH !!! Firmware write corrupted.\r\n");
        printf("[IAP] Do NOT jump! Retry Serial IAP with KEY0 + Reset.\r\n");
        return -1;
    }
    printf("[IAP] CRC MATCH - Firmware integrity confirmed.\r\n");

    /* IAP state/packet handling. */
    printf("[IAP] Updating parameter area (0x%08X)...\r\n", PARAM_FLASH_START);
    memset(&g_param_buf_iap, 0xFF, sizeof(g_param_buf_iap));

    int load_ok = FlashParam_Load(&g_param_buf_iap);
    if (load_ok != 0) {
        /* IAP state/packet handling. */
        printf("[IAP] Param area uninitialized, creating new param structure.\r\n");
        g_param_buf_iap.fw_ver_major    = 1;
        g_param_buf_iap.fw_ver_minor    = 0;
        g_param_buf_iap.fw_ver_patch    = 0;
        g_param_buf_iap.fw_build_num    = 1;
        g_param_buf_iap.boot_count      = 0;
        strncpy(g_param_buf_iap.device_id,       "dev001",      sizeof(g_param_buf_iap.device_id) - 1);
        strncpy(g_param_buf_iap.mqtt_topic_prefix,"iot/dev001", sizeof(g_param_buf_iap.mqtt_topic_prefix) - 1);
    }

    /* IAP state/packet handling. */
    g_param_buf_iap.fw_size_bytes    = g_total_size;      /* IAP state/packet handling. */
    g_param_buf_iap.fw_crc32         = crc_flash;         /* IAP state/packet handling. */

    /* IAP state/packet handling. */
    if (g_param_buf_iap.fw_build_num == 0xFFFF || g_param_buf_iap.fw_build_num == 0xFFFE) {
        g_param_buf_iap.fw_build_num = 1;
    } else {
        g_param_buf_iap.fw_build_num = (uint16_t)(g_param_buf_iap.fw_build_num + 1);
    }

    /* IAP state/packet handling. */
    g_param_buf_iap.last_ota_result  = 0;   /* IAP state/packet handling. */

    /* IAP state/packet handling. */

    /* IAP state/packet handling. */
    ret = FlashParam_Save(&g_param_buf_iap);
    if (ret != 0) {
        printf("[IAP] !!! Param Save FAIL (but firmware already written)!\r\n");
        /* IAP state/packet handling. */
    } else {
        printf("[IAP] Param Save OK. build=%u, fw_crc=0x%08X, fw_size=%uB\r\n",
               g_param_buf_iap.fw_build_num, crc_flash, g_total_size);
    }

    /* IAP state/packet handling. */
    (void)FlashParam_ClearOtaRequest();   /* IAP state/packet handling. */
    printf("[IAP] OTA request flag cleared.\r\n");

    /* IAP state/packet handling. */
    printf("\r\n[IAP] ===== ALL DONE. Soft-reset in 300ms... =====\r\n");
    HAL_Delay(300);      /* IAP state/packet handling. */

    NVIC_SystemReset(); /* IAP state/packet handling. */

    return 0;           /* IAP state/packet handling. */
}
