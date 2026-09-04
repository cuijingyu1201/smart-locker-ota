#include "touch.h"
#include "lcd.h"

_m_tp_dev tp_dev;

/* ================= DWT 内核周期计数器实现微秒延时（不占用 SysTick/TIM） ================= */
#define DWT_CYCCNT           (*(volatile uint32_t *)0xE0001004UL)
#define DWT_CTRL             (*(volatile uint32_t *)0xE0001000UL)
#define DEM_CR               (*(volatile uint32_t *)0xE000EDFCUL)
#define DEM_CR_TRCENA        (1UL << 24)
#define DWT_CTRL_CYCCNTENA   (1UL << 0)

static void tp_dwt_init(void)
{
    DEM_CR   |= (uint32_t)DEM_CR_TRCENA;   /* 使能 DWT 跟踪 */
    DWT_CYCCNT = 0U;                       /* 清零周期计数器 */
    DWT_CTRL |= (uint32_t)DWT_CTRL_CYCCNTENA;  /* 启动 CYCCNT */
}

/* 微秒级延时；无符号减法天然处理 32 位回绕（72MHz 下约 59.6 秒回绕一次） */
static void tp_delay_us(uint32_t us)
{
    uint32_t start = DWT_CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT_CYCCNT - start) < ticks) { }
}

/* ================= 软件 SPI：向 XPT2046 写 1 字节命令 ================= */
static void tp_write_byte(uint8_t data)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        T_MOSI((data & 0x80) ? 1 : 0);
        data <<= 1;
        T_CLK(0);
        tp_delay_us(1);
        T_CLK(1);      /* 上升沿有效 */
    }
}

/* 读一次 ADC（12bit）。cmd: 0x90 读Y通道 / 0xD0 读X通道 */
static uint16_t tp_read_ad(uint8_t cmd)
{
    uint8_t  i;
    uint16_t num = 0;

    T_CLK(0);
    T_MOSI(0);
    T_CS(0);                 /* 选中芯片 */
    tp_write_byte(cmd);
    tp_delay_us(6);          /* ADS7846/XPT2046 转换时间约 6us */
    T_CLK(0); tp_delay_us(1);
    T_CLK(1); tp_delay_us(1);  /* 第 1 个时钟，丢弃 BUSY */
    T_CLK(0);

    for (i = 0; i < 16; i++) {   /* 读 16 位，仅高 12 位有效 */
        num <<= 1;
        T_CLK(0); tp_delay_us(1);
        T_CLK(1);
        if (T_MISO) num++;
    }
    num >>= 4;              /* 取高 12 位 */
    T_CS(1);                /* 释放片选 */
    return num;
}

/* ============ 滤波：连续读 5 次，排序后去头去尾，取中间平均 ============ */
#define TP_READ_TIMES   5
#define TP_LOST_VAL     1

static uint16_t tp_read_xoy(uint8_t cmd)
{
    uint16_t buf[TP_READ_TIMES];
    uint16_t i, j, temp, sum = 0;

    for (i = 0; i < TP_READ_TIMES; i++) buf[i] = tp_read_ad(cmd);

    for (i = 0; i < TP_READ_TIMES - 1; i++)       /* 冒泡排序 */
        for (j = i + 1; j < TP_READ_TIMES; j++)
            if (buf[i] > buf[j]) { temp = buf[i]; buf[i] = buf[j]; buf[j] = temp; }

    for (i = TP_LOST_VAL; i < TP_READ_TIMES - TP_LOST_VAL; i++) sum += buf[i];
    return sum / (TP_READ_TIMES - 2U * TP_LOST_VAL);
}

/* 读 X/Y（方向适配：竖屏/横屏交换通道） */
static void tp_read_xy(uint16_t *x, uint16_t *y)
{
    uint16_t xv, yv;
    if (tp_dev.touchtype & 0x01) {     /* 横屏 */
        xv = tp_read_xoy(0x90);
        yv = tp_read_xoy(0xD0);
    } else {                           /* 竖屏 */
        xv = tp_read_xoy(0xD0);
        yv = tp_read_xoy(0x90);
    }
    *x = xv;
    *y = yv;
}

/* 双次读取一致性滤波：两次偏差在 50 以内才采信 */
#define TP_ERR_RANGE   50
static uint8_t tp_read_xy2(uint16_t *x, uint16_t *y)
{
    uint16_t x1, y1, x2, y2;
    tp_read_xy(&x1, &y1);
    tp_read_xy(&x2, &y2);
    if (((x2 <= x1 && x1 < x2 + TP_ERR_RANGE) || (x1 <= x2 && x2 < x1 + TP_ERR_RANGE)) &&
        ((y2 <= y1 && y1 < y2 + TP_ERR_RANGE) || (y1 <= y2 && y2 < y1 + TP_ERR_RANGE))) {
        *x = (x1 + x2) / 2U;
        *y = (y1 + y2) / 2U;
        return 1;
    }
    return 0;
}

/* ================= 初始化 ================= */
uint8_t tp_init(void)
{
    GPIO_InitTypeDef gi = {0};

    /* 时钟（FSMC 已开 GPIOF，这里重复使能无害，保证 GPIOB 也开） */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* 输入：T_PEN(PF10)、T_MISO(PB2)，上拉 */
    gi.Mode  = GPIO_MODE_INPUT;
    gi.Pull  = GPIO_PULLUP;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Pin   = T_PEN_GPIO_PIN;   HAL_GPIO_Init(T_PEN_GPIO_PORT, &gi);
    gi.Pin   = T_MISO_GPIO_PIN;  HAL_GPIO_Init(T_MISO_GPIO_PORT, &gi);

    /* 输出：T_MOSI(PF9)、T_CLK(PB1)、T_CS(PF11)，推挽上拉，默认高电平（空闲） */
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pin  = T_MOSI_GPIO_PIN;   HAL_GPIO_Init(T_MOSI_GPIO_PORT, &gi);
    gi.Pin  = T_CLK_GPIO_PIN;    HAL_GPIO_Init(T_CLK_GPIO_PORT,  &gi);
    gi.Pin  = T_CS_GPIO_PIN;     HAL_GPIO_Init(T_CS_GPIO_PORT,   &gi);
    T_MOSI(1); T_CLK(1); T_CS(1);

    tp_dwt_init();

    /* 方向取自 LCD（竖屏 dir=0 → touchtype=0） */
    tp_dev.touchtype = lcddev.dir & 0x01U;

    /* 默认校准参数 */
    tp_dev.xfac = TP_DEF_XFAC;
    tp_dev.yfac = TP_DEF_YFAC;
    tp_dev.xc   = TP_DEF_XC;
    tp_dev.yc   = TP_DEF_YC;
    tp_dev.sta  = 0;
    tp_dev.x = 0xFFFFU;
    tp_dev.y = 0xFFFFU;

    return 0;
}

/* ================= 扫描mode=0：转换为屏幕像素坐标 ================= */
uint8_t tp_scan(uint8_t mode)
{
    if (T_PEN == 0) {   /* 有触摸 */
        uint16_t xad = 0, yad = 0;
        if (tp_read_xy2(&xad, &yad)) {
            tp_dev.x_ad = xad;
            tp_dev.y_ad = yad;
            if (mode == 0) {
                int32_t sx = (int32_t)(((int32_t)xad - tp_dev.xc) / tp_dev.xfac) + lcddev.width  / 2;
                int32_t sy = (int32_t)(((int32_t)yad - tp_dev.yc) / tp_dev.yfac) + lcddev.height / 2;
                /* 边界钳制，防止越界写屏 */
                if (sx < 0) sx = 0;
                if (sy < 0) sy = 0;
                if (sx >= lcddev.width)  sx = lcddev.width  - 1;
                if (sy >= lcddev.height) sy = lcddev.height - 1;
                tp_dev.x = (uint16_t)sx;
                tp_dev.y = (uint16_t)sy;
            }
        }
        tp_dev.sta |= TP_PRES_DOWN;
        return 1;
    } else {
        tp_dev.sta &= ~TP_PRES_DOWN;
        return 0;
    }
}

