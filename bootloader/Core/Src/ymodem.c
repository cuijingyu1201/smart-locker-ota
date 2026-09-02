#include "ymodem.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* Keep the packet buffer out of the 1KB startup stack. */
static uint8_t y_data_buf[Y_PACKET_SIZE_STX];
static YmodemDebugInfo y_debug;

/*
 * Keep receiving while control bytes are transmitted. A polling receiver can
 * lose STX/sequence bytes when a sender starts immediately after ACK/C.
 */
#define Y_RX_RING_SIZE 4096U
#define Y_RX_RING_MASK (Y_RX_RING_SIZE - 1U)
static volatile uint8_t  y_rx_ring[Y_RX_RING_SIZE];
static volatile uint16_t y_rx_head;
static volatile uint16_t y_rx_tail;
static volatile uint32_t y_rx_error;
static volatile uint32_t y_rx_overrun;

const YmodemDebugInfo *Ymodem_GetDebugInfo(void)
{
    return &y_debug;
}

static void y_debug_uart(HAL_StatusTypeDef status)
{
    y_debug.last_hal_status = (uint32_t)status;
    y_debug.uart_error = (uint32_t)huart1.ErrorCode | y_rx_error;
    y_debug.uart_rx_state = (uint32_t)huart1.RxState;
    y_debug.uart_tx_state = (uint32_t)huart1.gState;
}

static int y_debug_frame_return(int ret)
{
    y_debug.last_frame_ret = (int32_t)ret;
    y_debug.uart_error = (uint32_t)huart1.ErrorCode | y_rx_error;
    y_debug.uart_rx_state = (uint32_t)huart1.RxState;
    y_debug.uart_tx_state = (uint32_t)huart1.gState;
    return ret;
}

static void y_rx_start(void)
{
    __disable_irq();
    y_rx_head = 0;
    y_rx_tail = 0;
    y_rx_error = 0;
    y_rx_overrun = 0;

    while (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) != RESET) {
        (void)huart1.Instance->DR;
    }
    __HAL_UART_CLEAR_OREFLAG(&huart1);

    HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
    __HAL_UART_ENABLE_IT(&huart1, UART_IT_ERR);
    __enable_irq();
}

static void y_rx_discard_pending(void)
{
    __disable_irq();
    y_rx_tail = y_rx_head;
    y_rx_overrun = 0;
    __enable_irq();
}

void Ymodem_UART_IRQHandler(void)
{
    uint32_t sr = huart1.Instance->SR;
    uint32_t error_flags = USART_SR_PE | USART_SR_FE |
                           USART_SR_NE | USART_SR_ORE;
    uint16_t next;
    uint8_t byte;

    if ((sr & USART_SR_RXNE) != 0U) {
        byte = (uint8_t)(huart1.Instance->DR & 0xFFU);
        if ((sr & error_flags) != 0U) {
            y_rx_error |= (sr & error_flags);
        }
        next = (uint16_t)((y_rx_head + 1U) & Y_RX_RING_MASK);
        if (next == y_rx_tail) {
            y_rx_overrun = 1U;
            y_rx_error |= HAL_UART_ERROR_ORE;
        } else {
            y_rx_ring[y_rx_head] = byte;
            y_rx_head = next;
        }
    } else if ((sr & error_flags) != 0U) {
        (void)huart1.Instance->DR;
        y_rx_error |= (sr & error_flags);
    }
}

uint16_t Ymodem_CRC16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0;
    uint16_t i;
    for (i = 0; i < len; ++i) {
        uint8_t bit;
        crc ^= (uint16_t)data[i] << 8;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                   : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void y_send_byte(uint8_t ch)
{
    (void)HAL_UART_Transmit(&huart1, &ch, 1, 100);
}

static HAL_StatusTypeDef y_recv_byte(uint8_t *ch, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint16_t tail;

    for (;;) {
        if (y_rx_overrun != 0U) {
            y_rx_discard_pending();
            y_debug_uart(HAL_ERROR);
            return HAL_ERROR;
        }
        if (y_rx_head != y_rx_tail) {
            tail = y_rx_tail;
            *ch = y_rx_ring[tail];
            y_rx_tail = (uint16_t)((tail + 1U) & Y_RX_RING_MASK);
            return HAL_OK;
        }
        if (timeout_ms != HAL_MAX_DELAY &&
            (HAL_GetTick() - start) >= timeout_ms) {
            y_debug_uart(HAL_TIMEOUT);
            return HAL_TIMEOUT;
        }
    }
}

static HAL_StatusTypeDef y_recv_bytes(uint8_t *data, uint16_t len,
                                      uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint16_t i;

    for (i = 0; i < len; ++i) {
        uint32_t elapsed = HAL_GetTick() - start;
        uint32_t remaining;
        if (elapsed >= timeout_ms) {
            return HAL_TIMEOUT;
        }
        remaining = timeout_ms - elapsed;
        if (y_recv_byte(&data[i], remaining) != HAL_OK) {
            return HAL_ERROR;
        }
    }
    return HAL_OK;
}

/* 0 = SOH, 1 = STX, -1 = EOT, -2 = malformed/timeout, -3 = CANx2. */
static int y_recv_frame(uint8_t *data, uint16_t *len, uint8_t *seq)
{
    uint8_t header;
    uint8_t seq_neg;
    uint8_t crc_hi;
    uint8_t crc_lo;
    uint16_t crc_rx;
    uint16_t crc_calc;
    uint16_t packet_len;

    ++y_debug.frame_count;
    y_debug.last_header = 0;
    y_debug.last_seq = 0;
    y_debug.last_seq_neg = 0;
    y_debug.last_data_len = 0;
    y_debug.last_crc_rx = 0;
    y_debug.last_crc_calc = 0;
    memset(y_debug.last_data_preview, 0, sizeof(y_debug.last_data_preview));

    if (y_recv_byte(&header, Y_TIMEOUT_PACKET) != HAL_OK) {
        y_rx_discard_pending();
        y_debug.last_header = 0xFF;
        printf("[YMODEM] Frame-start timeout\r\n");
        return y_debug_frame_return(-2);
    }
    y_debug.last_header = header;
    if (header == Y_EOT) {
        return y_debug_frame_return(-1);
    }
    if (header == Y_CAN) {
        uint8_t second;
        if (y_recv_byte(&second, Y_TIMEOUT_BYTE) == HAL_OK && second == Y_CAN) {
            return y_debug_frame_return(-3);
        }
        y_rx_discard_pending();
        printf("[YMODEM] Single CAN received\r\n");
        return y_debug_frame_return(-2);
    }
    if (header != Y_SOH && header != Y_STX) {
        y_rx_discard_pending();
        printf("[YMODEM] Invalid frame start: 0x%02X\r\n", header);
        return y_debug_frame_return(-2);
    }
    seq_neg = 0;
    if (y_recv_byte(seq, Y_TIMEOUT_BYTE) != HAL_OK ||
        y_recv_byte(&seq_neg, Y_TIMEOUT_BYTE) != HAL_OK) {
        y_debug.last_seq = *seq;
        y_debug.last_seq_neg = seq_neg;
        y_rx_discard_pending();
        printf("[YMODEM] Header sequence timeout\r\n");
        return y_debug_frame_return(-2);
    }
    y_debug.last_seq = *seq;
    y_debug.last_seq_neg = seq_neg;
    if (*seq != (uint8_t)~seq_neg) {
        y_rx_discard_pending();
        printf("[YMODEM] Bad sequence complement: seq=%u neg=0x%02X\r\n",
               *seq, seq_neg);
        return y_debug_frame_return(-2);
    }

    packet_len = (header == Y_SOH) ? Y_PACKET_SIZE_SOH : Y_PACKET_SIZE_STX;
    {
        HAL_StatusTypeDef status = y_recv_bytes(data, packet_len,
                                                Y_TIMEOUT_PACKET);
        if (status != HAL_OK) {
            y_rx_discard_pending();
            y_debug.last_data_len = packet_len;
            printf("[YMODEM] Data timeout: seq=%u len=%u\r\n", *seq, packet_len);
            return y_debug_frame_return(-2);
        }
    }
    y_debug.last_data_len = packet_len;
    memcpy(y_debug.last_data_preview, data,
           (packet_len < sizeof(y_debug.last_data_preview)) ?
           packet_len : sizeof(y_debug.last_data_preview));
    if (y_recv_byte(&crc_hi, Y_TIMEOUT_BYTE) != HAL_OK ||
        y_recv_byte(&crc_lo, Y_TIMEOUT_BYTE) != HAL_OK) {
        y_rx_discard_pending();
        printf("[YMODEM] CRC bytes timeout: seq=%u\r\n", *seq);
        return y_debug_frame_return(-2);
    }
    crc_rx = ((uint16_t)crc_hi << 8) | crc_lo;
    crc_calc = Ymodem_CRC16(data, packet_len);
    y_debug.last_crc_rx = crc_rx;
    y_debug.last_crc_calc = crc_calc;
    if (crc_rx != crc_calc) {
        y_rx_discard_pending();
        printf("[YMODEM] CRC mismatch: seq=%u rx=0x%04X calc=0x%04X\r\n",
               *seq, crc_rx, crc_calc);
        return y_debug_frame_return(-2);
    }
    *len = packet_len;
    return y_debug_frame_return((header == Y_SOH) ? 0 : 1);
}

/* Parse the standard Ymodem block-0 "name\\0size\\0" header. */
static int y_parse_head_frame(const uint8_t *data, uint32_t *size)
{
    const uint8_t *p = data;
    const uint8_t *end = data + Y_PACKET_SIZE_SOH;
    uint32_t value = 0;
    uint8_t digit_count = 0;

    if (data[0] == 0 || data[0] == Y_FILL) {
        return 1; /* empty block-0 terminates a Ymodem batch */
    }
    while (p < end && *p != 0) {
        ++p;
    }
    if (p == end) {
        return -1;
    }
    ++p;
    /* Accept senders that put padding whitespace before the size field. */
    while (p < end && (*p == ' ' || *p == '\t')) {
        ++p;
    }
    while (p < end && *p != 0) {
        uint32_t digit;
        /* Some Tera Term versions leave one space after the decimal size. */
        if (*p == ' ' || *p == '\t') {
            break;
        }
        if (*p < '0' || *p > '9' || value > (0xFFFFFFFFUL / 10UL)) {
            return -1;
        }
        digit = (uint32_t)(*p - '0');
        if (value > ((0xFFFFFFFFUL - digit) / 10UL)) {
            return -1;
        }
        value = value * 10UL + digit;
        ++digit_count;
        ++p;
    }
    if (digit_count == 0) {
        return -1;
    }
    *size = value;
    return 0;
}

static void y_cancel(void)
{
    y_send_byte(Y_CAN);
    y_send_byte(Y_CAN);
}

int Ymodem_Receive(uint8_t *app_buf, uint32_t max_buf_size,
                   uint32_t *received_size, YmodemPacketCB on_packet)
{
    uint16_t data_len = 0;
    uint8_t seq = 0;
    uint8_t expected_seq = 1;
    uint8_t last_acked_seq = 0;
    uint8_t first_packet_done = 0;  /* 替代 expected_seq != 1 判断初始状态，防序号回绕误判 */
    uint32_t total_received = 0;
    uint32_t file_total_size = 0;
    uint32_t errors = 0;
    int ret;
    int head_ok = 0;

    memset(&y_debug, 0, sizeof(y_debug));
    y_debug.last_frame_ret = 0x7FFFFFFF;
    y_debug.header_parse_result = 0x7FFFFFFF;

    if (received_size != NULL) {
        *received_size = 0;
    }
    if (max_buf_size == 0U || (app_buf == NULL && on_packet == NULL)) {
        return Y_ERR_SIZE;
    }

    printf("[YMODEM] RX build %s (CRC16/XMODEM, SOH/STX)\r\n",
           YMODEM_RX_BUILD);
    y_rx_start();

    /* Sender-side tools commonly need several seconds before responding. */
    while (!head_ok && errors < Y_INIT_RETRY_MAX) {
        y_debug.phase = 1;
        y_send_byte(Y_C_CHAR);
        ret = y_recv_frame(y_data_buf, &data_len, &seq);
        if ((ret == 0 || ret == 1) && seq == 0 &&
            (data_len == Y_PACKET_SIZE_SOH || data_len == Y_PACKET_SIZE_STX)) {
            int parse_result = y_parse_head_frame(y_data_buf, &file_total_size);
            y_debug.header_file_size = file_total_size;
            y_debug.header_parse_result = parse_result;
            y_debug.header_frame_header = y_debug.last_header;
            y_debug.header_seq = seq;
            y_debug.header_data_len = data_len;
            memcpy(y_debug.header_preview, y_data_buf,
                   sizeof(y_debug.header_preview));
            if (parse_result == 1) {
                y_send_byte(Y_ACK);
                return Y_OK;
            }
            if (parse_result < 0 || file_total_size == 0U ||
                file_total_size > max_buf_size) {
                printf("[YMODEM] Header rejected: seq=%u len=%u parse=%d size=%lu bytes=%02X %02X %02X %02X\r\n",
                       seq, data_len, parse_result, (unsigned long)file_total_size,
                       y_data_buf[0], y_data_buf[1], y_data_buf[2], y_data_buf[3]);
                y_cancel();
                return (parse_result < 0) ? Y_ERR_CRC : Y_ERR_SIZE;
            }
            y_debug.header_accepted = 1;
            /*
             * Print before ACK/C. A sender may start the first data frame
             * immediately after C; any diagnostic text after C would leave
             * the MCU polling TX while the UART RX register can overrun.
             */
            printf("[YMODEM] Header OK: seq=%u len=%u file_size=%lu bytes\r\n",
                   seq, data_len, (unsigned long)file_total_size);
            y_send_byte(Y_ACK);
            y_send_byte(Y_C_CHAR);
            head_ok = 1;
            break;
        }
        if (ret == -3) {
            return Y_ERR_CANCEL;
        }
        if (ret == 0 || ret == 1) {
            printf("[YMODEM] Unexpected header packet: ret=%d seq=%u len=%u\r\n",
                   ret, seq, data_len);
        }
        if (ret == -2) {
            y_debug.retry_count = errors + 1U;
            printf("[YMODEM] Header wait/reject retry %lu/%u\r\n",
                   (unsigned long)(errors + 1U), Y_INIT_RETRY_MAX);
        }
        /* A valid sender can retransmit after NAK; do not wait for the next
         * periodic 'C' when the first block was damaged. */
        y_send_byte(Y_NAK);
        ++errors;
    }
    if (!head_ok) {
        y_cancel();
        return Y_ERR_TIMEOUT;
    }

    errors = 0;
    for (;;) {
        uint16_t write_len;
        IWDG->KR = 0xAAAAU;   /* 喂狗：Ymodem 传输 464KB 可能 40+ 秒 */
        y_debug.phase = 2;
        ret = y_recv_frame(y_data_buf, &data_len, &seq);
        if (ret == -1) {
            /* Ymodem uses NAK/EOT, then ACK. Accept either one or two EOTs. */
            y_send_byte(errors == 0 ? Y_NAK : Y_ACK);
            if (errors == 0) {
                uint8_t second;
                HAL_StatusTypeDef second_status = y_recv_byte(&second, Y_TIMEOUT_EOT);
                if (second_status == HAL_OK && second == Y_EOT) {
                    y_send_byte(Y_ACK);
                } else if (second_status == HAL_TIMEOUT) {
                    /* A few senders use a single EOT. The payload is already
                     * complete and verified, so accept that form as well. */
                    y_send_byte(Y_ACK);
                } else if (second_status == HAL_OK && second == Y_CAN) {
                    if (received_size != NULL) {
                        *received_size = total_received;
                    }
                    y_cancel();
                    return Y_ERR_CANCEL;
                } else {
                    /*
                     * Some senders put an extra control byte after a single
                     * EOT. Do not discard the valid payload just because the
                     * optional second-EOT exchange is non-standard. The
                     * size check below still rejects a genuinely premature
                     * EOT.
                     */
                    y_rx_discard_pending();
                    y_send_byte(Y_ACK);
                }
            }
            break;
        }
        if (ret == -3) {
            return Y_ERR_CANCEL;
        }
        if (ret == -2) {
            y_debug.retry_count = errors + 1U;
            if (++errors > Y_PACKET_RETRY_MAX) {
                y_cancel();
                return Y_ERR_CRC;
            }
            y_send_byte(Y_NAK);
            continue;
        }
        errors = 0;

        /* A retransmitted packet is already committed: ACK it without writing twice. */
        if (seq == last_acked_seq && first_packet_done) {
            y_send_byte(Y_ACK);
            continue;
        }
        if (seq != expected_seq || total_received >= file_total_size) {
            y_send_byte(Y_NAK);
            continue;
        }
        write_len = data_len;
        if ((uint32_t)write_len > (file_total_size - total_received)) {
            write_len = (uint16_t)(file_total_size - total_received);
        }
        if (write_len == 0U) {
            y_cancel();
            return Y_ERR_SIZE;
        }

        if (on_packet != NULL) {
            if (on_packet(total_received, y_data_buf, write_len) != 0) {
                y_cancel();
                return Y_ERR_CANCEL;
            }
        } else {
            if (total_received + write_len > max_buf_size) {
                y_cancel();
                return Y_ERR_SIZE;
            }
            memcpy(app_buf + total_received, y_data_buf, write_len);
        }
        total_received += write_len;
        last_acked_seq = seq;
        expected_seq = (uint8_t)(expected_seq + 1U);
        first_packet_done = 1;  /* 标记已处理过至少一个包，不再处于初始状态 */
        y_send_byte(Y_ACK);
    }

    if (received_size != NULL) {
        *received_size = total_received;
    }
    if (total_received != file_total_size) {
        return Y_ERR_SIZE;
    }

    /* Final empty block-0 is part of standard Ymodem batch termination. */
    y_debug.phase = 3;
    y_send_byte(Y_C_CHAR);
    {
        int end_ret = y_recv_frame(y_data_buf, &data_len, &seq);
        if (end_ret == 0 && seq == 0 && data_len == Y_PACKET_SIZE_SOH && y_data_buf[0] == 0) {
            /* 合法结束帧 */
            y_send_byte(Y_ACK);
        } else if (end_ret == -2) {
            /* 超时：PC 端可能已断开，数据已完整，打印警告但返回成功 */
            printf("[YMODEM] End-frame timeout (data OK, %lu bytes)\r\n",
                   (unsigned long)total_received);
        } else {
            /* 非法帧：发 NAK 让 PC 重试一次，重试仍失败则放弃 */
            y_send_byte(Y_NAK);
            end_ret = y_recv_frame(y_data_buf, &data_len, &seq);
            if (end_ret == 0 && seq == 0 && data_len == Y_PACKET_SIZE_SOH && y_data_buf[0] == 0) {
                y_send_byte(Y_ACK);
            } else {
                printf("[YMODEM] End-frame invalid (data OK, %lu bytes)\r\n",
                       (unsigned long)total_received);
            }
        }
    }
    return Y_OK;
}
