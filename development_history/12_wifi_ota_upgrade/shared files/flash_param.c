#include "flash_param.h"
#include "flash_if.h"   /* 复用 flash_if.c 的 FLASH_ErasePage / FLASH_WriteBuf / FLASH_ReadWord */
#include <stdio.h>
#include <string.h>

/* ================================================================
 *   CRC32 查表法实现（IEEE 802.3 多项式 0xEDB88320）
 *   256 项的表，用 const 放在 Flash（只读，不占 RAM）
 * ================================================================ */
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3B1A, 0xB2BD0B8C, 0x2BB45A26, 0x5CB36AB0, 0xCCB1EDA2, 0xBBB6DF34, 0x22BD8C8E, 0x55BABD18,
    0xAC03C7A4, 0xDB04F732, 0x420DA688, 0x350A961E, 0xAB6E03BD, 0xDC69332B, 0x45606291, 0x32675207,
    0xA2D04F96, 0xD5D77F00, 0x4CDE2EBA, 0x3BD91E2C, 0x5BD8B8F, 0xD2DBBB19, 0x44B20AA3, 0x33B53A35,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

/* 这个表是预计算好的，不需要 InitTable，直接用就行。保留 InitTable 函数是为了接口统一。 */
void CRC32_InitTable(void)
{
    /* 表已经用 const 写死在 Flash 里了，啥也不用做 */
}

uint32_t CRC32_Calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;   /* CRC32 初始值固定为 0xFFFFFFFF */
    for (uint32_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFFUL;     /* 最后按位取反，得到标准 CRC32 值 */
}

uint32_t CRC32_CalcAppFlash(void)
{
    /* 逐字节读 APP_FLASH_START 到 APP_FLASH_END - 1，计算 CRC32
     * 注意：必须读实际固件大小 fw_size_bytes 而不是整个 APP 区
     * 这里先用整个 APP 区算，D10/D12 写固件时会把 fw_size_bytes 写入参数区，到时候按精确大小算 */
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

    /* 1. 在 RAM 里构造一份完整参数，补齐 magic、crc、tail */
    flash_param_t tmp;
    memcpy(&tmp, p_in, sizeof(flash_param_t));
    tmp.magic_header = MAGIC_PARAM_HEADER;
    tmp.tail_marker  = 0xDEADBEEFUL;
    tmp.struct_crc32 = param_calc_struct_crc(&tmp);

    /* 2. 擦除参数区所在的 Flash 页 */
    if (FLASH_ErasePage(PARAM_FLASH_START) != HAL_OK) {
        return -1;
    }

    /* 3. 按 4 字节对齐写入整个 4096 字节参数区 */
    if (FLASH_WriteBuf(PARAM_FLASH_START, (uint8_t *)&tmp, sizeof(flash_param_t)) != HAL_OK) {
        return -1;
    }

    return 0;
}

/* ================================================================
 *   FlashParam_SetOtaRequest：只写 OTA 请求字段（不整页擦，用按字写优化）
 *   升级完成后 APP 调用这个函数，然后 NVIC_SystemReset() 复位进 Bootloader
 * ================================================================ */
int FlashParam_SetOtaRequest(uint32_t new_fw_crc32, uint32_t new_fw_size,
                            uint16_t ver_major, uint16_t ver_minor,
                            uint16_t ver_patch, uint16_t build_num)
{
    /* 注意：Flash 只能把 1 写 0，不能把 0 写 1。
     * 如果字段原本有非 0xFF 的值，按字写会出错。所以这里：
     *  1. 先读参数区到 RAM
     *  2. 在 RAM 里改字段
     *  3. 用整包 Save（擦+写）保证正确
     *  （优化版 D12 再做：如果原字段都是 0xFF 可以直接写，省一次擦写）
     */
    flash_param_t p;
    int ret = FlashParam_Load(&p);
    if (ret != 0) {
        /* 参数区未初始化：先填默认值，再写入 OTA 请求 */
        memset(&p, 0xFF, sizeof(p));   /* 先全 0xFF（Flash擦除后默认值） */
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

int FlashParam_ClearOtaRequest(void)
{
    /* 逻辑和 SetOtaRequest 一样：读 → 清字段 → 整包写回 */
    flash_param_t p;
    if (FlashParam_Load(&p) != 0) {
        return 0;   /* 参数区本来就无效，不用清 */
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
    printf("  ota_new_version = %u.%u.%u.%u\r\n",
           p->ota_new_fw_ver_major, p->ota_new_fw_ver_minor,
           p->ota_new_fw_ver_patch, p->ota_new_fw_build_num);
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
