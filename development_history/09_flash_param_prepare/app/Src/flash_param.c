#include "flash_param.h"
#include "flash_if.h"   /* 复用 flash_if.c 的 FLASH_ErasePage / FLASH_WriteBuf / FLASH_ReadWord */
#include <stdio.h>
#include <string.h>

/* ================================================================
 *   CRC32 运行时生成表法（IEEE 802.3 多项式 0xEDB88320）
 *   优点：不需要死记 256 项，复制即正确，100% 不会数错
 *   代价：启动时 CRC32_InitTable() 花 ~50 微秒（完全可接受），表占 1KB RAM
 * ================================================================ */
/* CRC32 运行时查表法（IEEE 802.3，多项式 0xEDB88320） */
static uint32_t crc32_table[256];   /* 256 项 CRC 余数表，上电 Init 一次即可 */
static flash_param_t g_param_buf_internal; /* 参数区内部缓冲区，放 BSS 段，避免栈溢出 */


/* 用多项式 0xEDB88320 生成 0x00~0xFF 对应的 256 个表项（上电调用 1 次） */
void CRC32_InitTable(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t bit = 0; bit < 8; bit++) {
            /* 最低位为 1 则异或多项式，否则只右移 */
            crc = (crc & 1u) ? ((crc >> 1) ^ CRC32_POLYNOMIAL) : (crc >> 1);
        }
        crc32_table[i] = crc;
    }
}

/* 计算任意内存块的标准 CRC32（初始值 0xFFFFFFFF，最后取反） */
uint32_t CRC32_Calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* 计算整片 APP 区（0x08008000 起的 464KB）的 CRC32 */
uint32_t CRC32_CalcAppFlash(void)
{
    return CRC32_Calc((const uint8_t *)APP_FLASH_START, APP_FLASH_SIZE);
}

/* ================================================================
 *   参数区内部辅助：计算结构体 CRC32（跳过 magic_header 和 struct_crc32 字段本身）
 * ================================================================ */
static uint32_t param_calc_struct_crc(const flash_param_t *p)
{
    /* CRC 计算范围：从 offsetof(magic_header)之后的字段 = 地址+8 开始，直到参数区末尾倒数16字节（不含Tail的冗余CRC）*/
    const uint8_t *start = (const uint8_t *)p + 8;      /* 跳过 magic_header(4B) + struct_crc32(4B) */
    const uint8_t *end   = (const uint8_t *)p + 0xFF0;  /* 到 Tail 之前停止（Tail单独校验）*/
    return CRC32_Calc(start, (uint32_t)(end - start));
}

/* ================================================================
 *   FlashParam_Load：从 Flash 参数区读到 RAM p_out
 *   返回 0=成功（magic正确+CRC正确） -1=失败（参数区是全新或损坏）
 * ================================================================ */
int FlashParam_Load(flash_param_t *p_out)
{
    if (p_out == NULL) return -1;

    /* 1. 直接从 Flash 映射地址整块 memcpy 到 RAM（Flash 读不用解锁）*/
    memcpy(p_out, (const void *)PARAM_FLASH_START, sizeof(flash_param_t));

    /* 2. 检查 magic_header */
    if (p_out->magic_header != MAGIC_PARAM_HEADER) {
        return -1;  /* 参数区从未被初始化过（全0xFF或垃圾）*/
    }

    /* 3. 检查 tail_marker，防止半页写（写Flash中途断电导致只写了前半部分）*/
    if (p_out->tail_marker != 0xDEADBEEFUL) {
        return -1;
    }

    /* 4. 校验 struct_crc32（防止单个字段被误改）*/
    uint32_t expected_crc = param_calc_struct_crc(p_out);
    if (expected_crc != p_out->struct_crc32) {
        return -1;  /* CRC 不匹配：字段损坏或结构变了 */
    }

    return 0;   /* 全部通过：参数区合法 */
}

/* ================================================================
 *   FlashParam_Save：整包写入参数区（自动先擦除）
 * ================================================================ */
int FlashParam_Save(const flash_param_t *p_in)
{
    if (p_in == NULL) return -1;

    printf("[PARAM] Save start...\r\n");   /* ← 加这行 */

    memcpy(&g_param_buf_internal, p_in, sizeof(flash_param_t));
    g_param_buf_internal.magic_header = MAGIC_PARAM_HEADER;
    g_param_buf_internal.tail_marker  = 0xDEADBEEFUL;
    g_param_buf_internal.struct_crc32 = param_calc_struct_crc(&g_param_buf_internal);
    printf("[PARAM] CRC32 calc done: 0x%08X\r\n", g_param_buf_internal.struct_crc32);   

    printf("[PARAM] Erasing page @0x%08X...\r\n", PARAM_FLASH_START);   
    if (FLASH_ErasePage(PARAM_FLASH_START) != HAL_OK) {
        printf("[PARAM] Erase FAIL!\r\n");  
        return -1;
    }
    printf("[PARAM] Erase OK\r\n");  

    printf("[PARAM] Writing buffer...\r\n");  
    if (FLASH_WriteBuf(PARAM_FLASH_START, (uint8_t *)&g_param_buf_internal, sizeof(flash_param_t)) != HAL_OK) {
        printf("[PARAM] Write FAIL!\r\n");   
        return -1;
    }
    printf("[PARAM] Write OK\r\n");   

    return 0;
}
/* ================================================================
 *   FlashParam_SetOtaRequest：只写 OTA 请求字段（不整页擦，用按字写优化）
 *   升级完成后 APP 调用这个函数，然后 NVIC_SystemReset() 复位进 Bootloader
 * ================================================================ */
int FlashParam_SetOtaRequest(uint32_t new_fw_crc32, uint32_t new_fw_size)
{
    /* 注意：Flash 只能把 1 写 0，不能把 0 写 1。
     * 如果字段原本有非 0xFF 的值，按字写会出错。所以这里：
     *  1. 先读参数区到 RAM
     *  2. 在 RAM 里改字段
     *  3. 用整包 Save（擦+写）保证正确
     *  （优化版 D12 再做：如果原字段都是 0xFF 可以直接写，省一次擦写）
     */
    int ret = FlashParam_Load(&g_param_buf_internal);
    if (ret != 0) {
        /* 参数区未初始化：先填默认值，再写入 OTA 请求 */
        memset(&g_param_buf_internal, 0xFF, sizeof(g_param_buf_internal));   /* 先全 0xFF（Flash擦除后默认值） */
        g_param_buf_internal.boot_count = 0;
        g_param_buf_internal.fw_ver_major = 0;
        g_param_buf_internal.fw_ver_minor = 0;
        g_param_buf_internal.fw_ver_patch = 0;
    }
    g_param_buf_internal.ota_request_magic = MAGIC_OTA_REQUEST;
    g_param_buf_internal.ota_new_fw_crc32  = new_fw_crc32;
    g_param_buf_internal.ota_new_fw_size   = new_fw_size;
    return FlashParam_Save(&g_param_buf_internal);
}

int FlashParam_ClearOtaRequest(void)
{
    /* 逻辑和 SetOtaRequest 一样：读 → 清字段 → 整包写回 */
    if (FlashParam_Load(&g_param_buf_internal) != 0) {
        return 0;   /* 参数区本来就无效，不用清 */
    }
    g_param_buf_internal.ota_request_magic = 0x00000000;
    g_param_buf_internal.ota_new_fw_crc32  = 0xFFFFFFFF;
    g_param_buf_internal.ota_new_fw_size   = 0xFFFFFFFF;
    return FlashParam_Save(&g_param_buf_internal);
}

/* ================================================================
 *   FlashParam_Print：调试打印（D9 验证时调用）
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
    printf("  struct_version  = %lu\r\n", p->struct_version);
    printf("  fw_version      = %u.%u.%u (build %u)\r\n",
           p->fw_ver_major, p->fw_ver_minor, p->fw_ver_patch, p->fw_build_num);
    printf("  fw_size_bytes   = %lu (0x%08lX)\r\n", p->fw_size_bytes, p->fw_size_bytes);
    printf("  fw_crc32        = 0x%08X\r\n", p->fw_crc32);
    printf("  ota_req_magic   = 0x%08X (%c%c%c%c)\r\n", p->ota_request_magic,
           (p->ota_request_magic >> 0) & 0xFF, (p->ota_request_magic >> 8) & 0xFF,
           (p->ota_request_magic >> 16) & 0xFF, (p->ota_request_magic >> 24) & 0xFF);
    printf("  ota_new_fw_crc  = 0x%08X\r\n", p->ota_new_fw_crc32);
    printf("  ota_new_fw_size = %lu\r\n", p->ota_new_fw_size);
    printf("  ota_rollback    = %u\r\n", p->ota_rollback_count);
    printf("  boot_count      = %lu\r\n", p->boot_count);
    printf("  last_reset_reas = 0x%08X\r\n", p->last_reset_reason);
    printf("  last_ota_result = %lu\r\n", p->last_ota_result);
    printf("  device_id       = %.16s\r\n", p->device_id);
    printf("  mqtt_prefix     = %.32s\r\n", p->mqtt_topic_prefix);
    printf("  tail_marker     = 0x%08X\r\n", p->tail_marker);
    printf("  tail_crc32      = 0x%08X\r\n", p->tail_crc32);
    printf("=========================================\r\n\r\n");
}
