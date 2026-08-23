#ifndef __FLASH_PARTITION_H
#define __FLASH_PARTITION_H

#include "main.h"   // HAL 里已经有了 uint32_t/uint16_t 这些类型

/* ================================================================
 *   Flash 三分区地址定义（Bootloader / APP / 参数区）
 *   所有分区宏统一集中在这里，Bootloader 和 APP 两边都 include
 *   以后改分区大小只需要改这一个文件，两边同步
 * ================================================================ */

/* ---- Bootloader 区：前32KB ---- */
#define BL_FLASH_START        0x08000000UL
#define BL_FLASH_SIZE         0x00008000UL    /* 32KB */

/* ---- APP 区：Bootloader之后的464KB ---- */
#define APP_FLASH_START       0x08008000UL
#define APP_FLASH_SIZE        0x00074000UL    /* 464KB (512KB - 32KB - 4KB) */
#define APP_FLASH_END         (APP_FLASH_START + APP_FLASH_SIZE)  /* 0x0807EFFF */

/* ---- 参数区：最后一个扇区（4KB），Bootloader 和 APP 共享 ---- */
#define PARAM_FLASH_START     0x0807F000UL
#define PARAM_FLASH_SIZE      0x00001000UL   /* 4KB = 2 pages @ F103ZE (每页 2KB) */

/* ================================================================
 *   参数区 magic 定义（防止把垃圾数据当成有效参数）
 *   两个魔数用途不同：
 *     MAGIC_PARAM_HEADER = 0x504D5431 ("PMT1" = Param 1)
 *        → 表示整个参数区结构体有效（字段都有意义）
 *     MAGIC_OTA_REQUEST   = 0x4F544131 ("OTA1" = OTA Update Request 1)
 *        → 表示 APP 已经在 APP_FLASH_START 写好了新固件，Bootloader 请执行升级流程
 *        → （Hard Constraints 已强制要求这个值，必须遵守）
 * ================================================================ */
#define MAGIC_PARAM_HEADER    0x504D5431UL    /* "PMT1"，参数区有效标志 */
#define MAGIC_OTA_REQUEST     0x4F544131UL    /* "OTA1"，OTA升级请求标志，Hard Constraints规定 */
#define MAGIC_OLD_OTA_FLAG    0xA5A5A5A5UL    /* D8 用的旧标志，兼容保留，后续废弃 */

/* ================================================================
 *   CRC32 配置
 *   IEEE 802.3 多项式 0xEDB88320（Hard Constraints 强制要求）
 * ================================================================ */
#define CRC32_POLYNOMIAL      0xEDB88320UL

/* ================================================================
 *   flash_param_t —— 参数区结构体（大小必须 <= 4KB，4字节对齐）
 *   字段按 4 字节对齐排布，Bootloader 和 APP 两边都用这个结构体
 *   ⚠️ 任何字段顺序/大小的修改，都必须两边工程同时改 flash_partition.h
 * ================================================================ */
typedef struct __attribute__((packed, aligned(4))) {
    /* ---------- Header：前 12 字节固定 ---------- */
    uint32_t magic_header;        /* 0x00: MAGIC_PARAM_HEADER(0x504D5431)，参数区有效标志 */
    uint32_t struct_crc32;        /* 0x04: 整个结构体（除 magic_header 和 struct_crc32 自身外所有字节）的 CRC32 */
    uint32_t struct_version;      /* 0x08: 结构体格式版本=1，未来扩展用 */

    /* ---------- 固件版本信息（APP端写入，Bootloader端读出判断要不要升级）---------- */
    uint16_t fw_ver_major;        /* 0x0C: 主版本号，例 1 */
    uint16_t fw_ver_minor;        /* 0x0E: 次版本号，例 0 */
    uint16_t fw_ver_patch;        /* 0x10: 补丁版本，例 3 → 版本 1.0.3 */
    uint16_t fw_build_num;        /* 0x12: 构建号，每次编译+1，升级幂等判断用 */
    uint32_t fw_size_bytes;       /* 0x14: APP.bin 实际大小（字节），Ymodem/OTA接收完写入 */
    uint32_t fw_crc32;            /* 0x18: APP.bin 内容的 CRC32（校验固件完整性） */

    /* ---------- OTA 请求控制（APP 写入 OTA 请求，Bootloader 读取执行）---------- */
    uint32_t ota_request_magic;   /* 0x1C: MAGIC_OTA_REQUEST(0x4F544131) 表示有待升级固件 */
    uint32_t ota_new_fw_crc32;    /* 0x20: 新固件的 CRC32（写完新固件后 APP 写入） */
    uint32_t ota_new_fw_size;     /* 0x24: 新固件的大小（字节）*/
    uint8_t  ota_rollback_count;  /* 0x28: 连续回滚次数，超过3次自动锁死在串口IAP */
    uint8_t  rsvd[3];             /* 0x29~0x2B: 保留字节，凑4字节对齐 */

    /* ---------- 运行状态（APP 周期更新）---------- */
    uint32_t boot_count;          /* 0x2C: 上电次数，每次 APP 启动 +1 */
    uint32_t last_reset_reason;   /* 0x30: 最后一次复位原因，RCC->CSR 寄存器值，排障用 */
    uint32_t last_ota_result;     /* 0x34: 最后一次 OTA 结果，0=成功 1=CRC失败 2=写Flash失败 3=超时 */

    /* ---------- 可扩展的设备信息（面试加分项）---------- */
    char     device_id[16];       /* 0x38~0x47: 设备序列号，例 "dev001"，MQTT ClientID 从这里读 */
    char     mqtt_topic_prefix[32]; /* 0x48~0x67: MQTT 主题前缀，例 "iot/dev001"，改这里不用重编译 */
    uint8_t  reserved[0xF88];     /* 0x68~0xFEF: 预留，填充到 4080 字节 */

    /* ---------- Tail：最后 16 字节冗余校验 ---------- */
    uint32_t tail_marker;         /* 0xFF0: 0xDEADBEEF，用来快速判断参数区有没有写满 */
    uint32_t tail_crc32;          /* 0xFF4: 整包二次 CRC（可选，头部一次尾部一次） */
    uint32_t rsvd_tail1;          /* 0xFF8: 预留 */
    uint32_t rsvd_tail2;          /* 0xFFC: 预留 */
} flash_param_t;

/* 编译期结构体大小检查：必须正好是 4096 字节
 * C99 兼容写法（ARMCC V5 不支持 C11 的 _Static_assert）*/
#define STATIC_ASSERT_CONCAT_(a, b) a##b
#define STATIC_ASSERT_CONCAT(a, b) STATIC_ASSERT_CONCAT_(a, b)
#define STATIC_ASSERT(cond, msg) \
    typedef char STATIC_ASSERT_CONCAT(static_assert_, __LINE__)[(cond) ? 1 : -1]

STATIC_ASSERT(sizeof(flash_param_t) == 4096, param_size_must_be_4096);

#endif /* __FLASH_PARTITION_H */
		
		
		
	