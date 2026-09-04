#ifndef __TOUCH_H
#define __TOUCH_H

#include "main.h"

/* ============================================================
 * XPT2046 电阻触摸屏驱动（软件 SPI）— 精英板 V2 / 2.8寸 ILI9341
 * 引脚（均已核对，与 FSMC/USART 无冲突）：
 *   T_PEN  = PF10  触摸中断(输入上拉, 有触摸=低电平)
 *   T_CS   = PF11  片选(推挽输出)
 *   T_MISO = PB2   主机输入(输入上拉)
 *   T_MOSI = PF9   主机输出(推挽输出)
 *   T_CLK  = PB1   时钟(推挽输出)
 * ============================================================ */
#define T_PEN_GPIO_PORT    GPIOF
#define T_PEN_GPIO_PIN     GPIO_PIN_10
#define T_CS_GPIO_PORT     GPIOF
#define T_CS_GPIO_PIN      GPIO_PIN_11
#define T_MISO_GPIO_PORT   GPIOB
#define T_MISO_GPIO_PIN    GPIO_PIN_2
#define T_MOSI_GPIO_PORT   GPIOF
#define T_MOSI_GPIO_PIN    GPIO_PIN_9
#define T_CLK_GPIO_PORT    GPIOB
#define T_CLK_GPIO_PIN     GPIO_PIN_1

#define T_PEN   HAL_GPIO_ReadPin(T_PEN_GPIO_PORT,  T_PEN_GPIO_PIN)
#define T_MISO  HAL_GPIO_ReadPin(T_MISO_GPIO_PORT, T_MISO_GPIO_PIN)
#define T_MOSI(x)  HAL_GPIO_WritePin(T_MOSI_GPIO_PORT, T_MOSI_GPIO_PIN, (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define T_CLK(x)   HAL_GPIO_WritePin(T_CLK_GPIO_PORT,  T_CLK_GPIO_PIN,  (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define T_CS(x)    HAL_GPIO_WritePin(T_CS_GPIO_PORT,   T_CS_GPIO_PIN,   (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)

/* 2.8寸 240x320 电阻屏默认校准参数（出厂典型值；可用串口打印的原始AD值微调） */
#define TP_DEF_XFAC    15.25f   /* X方向 每像素AD数 ≈ (3850-190)/240 */
#define TP_DEF_YFAC    11.44f   /* Y方向 每像素AD数 ≈ (3850-190)/320 */
#define TP_DEF_XC      2020     /* X方向 中心AD值 */
#define TP_DEF_YC      2020     /* Y方向 中心AD值 */

#define TP_PRES_DOWN   0x8000U  /* 按下标志位 */

typedef struct {
    float    xfac;      /* X方向 每像素AD数 */
    float    yfac;      /* Y方向 每像素AD数 */
    short    xc;        /* X中心AD值 */
    short    yc;        /* Y中心AD值 */
    uint16_t x;         /* 当前屏幕X坐标(像素) */
    uint16_t y;         /* 当前屏幕Y坐标(像素) */
    uint16_t x_ad;      /* 调试: 原始X AD值(0~4095) */
    uint16_t y_ad;      /* 调试: 原始Y AD值(0~4095) */
    uint16_t sta;       /* bit15: 1按下/0松开 */
    uint8_t  touchtype; /* bit0: 0竖屏 1横屏（取自 lcddev.dir） */
} _m_tp_dev;

extern _m_tp_dev tp_dev;

uint8_t tp_init(void);          /* 初始化 GPIO+DWT+默认参数, 返回0成功 */
uint8_t tp_scan(uint8_t mode);  /* 扫描; mode=0转屏幕坐标; 返回1按下 0松开 */


#endif /* __TOUCH_H */

