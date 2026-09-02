#ifndef __YMODEM_H
#define __YMODEM_H

#include "main.h"   /* HAL 头文件，有 uint8_t/uint16_t/uint32_t/HAL_StatusTypeDef */

/* ================================================================
 *   Ymodem 协议控制字节（查官方协议文档即可，都是固定值）
 * ================================================================ */
#define Y_SOH       0x01    /* 128 字节小包帧头 */
#define Y_STX       0x02    /* 1024 字节大包帧头（我们主要用这个，快 8 倍） */
#define Y_EOT       0x04    /* 传输结束 */
#define Y_ACK       0x06    /* 确认收到 */
#define Y_NAK       0x15    /* 否认 + 要求重传 */
#define Y_CAN       0x18    /* 强制取消传输（连发 2 次） */
#define Y_C_CHAR    'C'     /* 0x43，请求 CRC 模式（必须先发这个才是 Ymodem-CRC） */
#define Y_FILL      0x1A    /* Ctrl+Z，最后一包不满 1024 字节时填充 */

/* 数据长度 */
#define Y_PACKET_SIZE_SOH   128
#define Y_PACKET_SIZE_STX   1024

/* Visible in the boot log so the programmed HEX can be identified. */
#define YMODEM_RX_BUILD     "20260823-r8-debug"

/* Snapshot of the last receive attempt, printed by IAP on any failure. */
typedef struct {
    int32_t  last_frame_ret;
    uint32_t phase;
    uint32_t frame_count;
    uint32_t retry_count;
    uint8_t  last_header;
    uint8_t  last_seq;
    uint8_t  last_seq_neg;
    uint16_t last_data_len;
    uint16_t last_crc_rx;
    uint16_t last_crc_calc;
    uint32_t header_file_size;
    int32_t  header_parse_result;
    uint8_t  header_accepted;
    uint8_t  header_frame_header;
    uint8_t  header_seq;
    uint16_t header_data_len;
    uint8_t  header_preview[16];
    uint8_t  last_data_preview[16];
    uint32_t last_hal_status;
    uint32_t uart_error;
    uint32_t uart_rx_state;
    uint32_t uart_tx_state;
} YmodemDebugInfo;

/* 超时配置（毫秒）*/
#define Y_TIMEOUT_INIT      5000    /* 等第一个 SOH 头帧的超时 */
#define Y_TIMEOUT_PACKET    3000    /* 等每一包数据的超时 */
#define Y_TIMEOUT_BYTE      1000    /* 等单个字节的超时 */
#define Y_TIMEOUT_EOT       2000    /* 等 EOT 的超时 */
#define Y_INIT_RETRY_MAX    5       /* 开头发 'C' 最多试 5 次（一次 5 秒） */
#define Y_PACKET_RETRY_MAX  10      /* 单个数据包最多重试次数 */

/* 函数返回值 */
#define Y_OK                0
#define Y_ERR_TIMEOUT       -1
#define Y_ERR_CRC           -2
#define Y_ERR_SEQ           -3
#define Y_ERR_CANCEL        -4
#define Y_ERR_SIZE          -5

/* ================================================================
 *   对外主函数：完整阻塞式收一整个文件
 *
 *   参数：
 *     app_buf       = 数据写入的 RAM 缓冲（D10 阶段流式写 Flash，实际用 NULL）
 *     max_buf_size  = 缓冲大小（用于防越界，流式时传 APP_FLASH_SIZE）
 *     received_size = 出参，实际收到的文件字节数
 *     on_packet     = 每收完一包的回调（用于流式写 Flash：偏移 + 数据 + 长度）
 *                     回调返回 0=继续，非0=中止传输
 *                     传 NULL=不使用回调，数据全部写进 app_buf
 *   返回：
 *     Y_OK(0) = 成功，其它负值 = 失败原因
 * ================================================================ */
typedef int (*YmodemPacketCB)(uint32_t offset, const uint8_t *data, uint16_t len);

int Ymodem_Receive(uint8_t *app_buf,
                   uint32_t max_buf_size,
                   uint32_t *received_size,
                   YmodemPacketCB on_packet);

const YmodemDebugInfo *Ymodem_GetDebugInfo(void);

/* USART1 RX interrupt entry point, called by the MCU IRQ handler. */
void Ymodem_UART_IRQHandler(void);

#endif /* __YMODEM_H */
