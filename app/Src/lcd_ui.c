#include "lcd_ui.h"
#include "app_lcd.h"       /* App_Lcd_Lock/Unlock */
#include "app_uart.h"
#include "lcd.h"            /* lcd_clear/lcd_show_string/lcd_fill/lcd_draw_rectangle */
#include "touch.h"          /* tp_scan/tp_dev */
#include "cabinet_fsm.h"    /* Cabinet_FSM_GetState/Cabinet_FSM_OpenRequest */
#include "app_dht11.h"      /* g_dht11_data/g_dht11_mutex_handle */
#include "app_sensor.h"     /* App_Sensor_GetDoor/GetItem */
#include "flash_param.h"    /* FlashParam 读取 last_open_ts */
#include <stdio.h>

/* ============================================================
 *  static 变量（全放 BSS 段，防栈溢出）
 * ============================================================ */
static ui_page_e s_current_page = PAGE_MAIN;
static cabinet_state_e s_last_drawn_state = (cabinet_state_e)0xFF;  /* 上次画的状态（避免重复刷屏）*/
static uint32_t s_last_temp_tick = 0;   /* 上次刷新温湿度的 tick */

/* 页面 1 按钮区域定义 */
#define BTN_OPEN_X1    40      /* 开柜按钮左边界 */
#define BTN_OPEN_X2    200     /* 开柜按钮右边界 */
#define BTN_OPEN_Y1    250     /* 开柜按钮上边界 */
#define BTN_OPEN_Y2    300     /* 开柜按钮下边界 */

/* ============================================================
 *  页面 1：主页面绘制
 * ============================================================ */
static void DrawPageMain(cabinet_state_e state, uint8_t force_redraw)
{
    /* 标题栏（顶部）*/
    if (force_redraw) {
        App_Lcd_Lock();
        lcd_clear(WHITE);
        lcd_show_string(10, 5, 240, 16, 16, (char *)"Locker CTRL v1.0.0", BLACK);
        lcd_show_string(10, 25, 240, 12, 12, (char *)"Status Monitor", GRAY);
        App_Lcd_Unlock();
    }

    /* 中间状态矩形（120×120，居中）*/
    uint16_t box_color;
    const char *state_str;
    switch (state) {
        case CAB_STATE_CLOSED:
            box_color = GREEN;
            state_str = "CLOSED";
            break;
        case CAB_STATE_OPENING:
        case CAB_STATE_WAIT_PICKUP:
        case CAB_STATE_TIMEOUT_CLOSING:
            box_color = RED;
            state_str = "OPEN";
            break;
        case CAB_STATE_FAULT:
            box_color = GRAY;
            state_str = "FAULT";
            break;
        default:
            box_color = GRAY;
            state_str = "UNKNOWN";
            break;
    }

    if (force_redraw || s_last_drawn_state != state) {
        App_Lcd_Lock();
        /* 画 120×120 状态矩形（居中：x=60~180, y=45~165）*/
        lcd_fill(60, 45, 180, 165, box_color);
        /* 矩形边框 */
        lcd_draw_rectangle(60, 45, 180, 165, BLACK);
        /* 居中文字 */
        lcd_show_string(80, 95, 100, 16, 16, (char *)state_str, WHITE);
        App_Lcd_Unlock();
        s_last_drawn_state = state;
    }

    /* 温湿度（左侧列，每 2 秒刷新一次）*/
    uint32_t now = osKernelGetTickCount();
    if (force_redraw || (now - s_last_temp_tick) >= 2000U) {
        s_last_temp_tick = now;
        DHT11_Data_t dht;
        if (g_dht11_mutex_handle != NULL &&
            osMutexAcquire(g_dht11_mutex_handle, 100) == osOK) {
            dht = g_dht11_data;
            osMutexRelease(g_dht11_mutex_handle);
        } else {
            dht.temp_int = 0;
            dht.humi_int = 0;
        }
        char buf[32];
        App_Lcd_Lock();
        snprintf(buf, sizeof(buf), "Temp: %dC", dht.temp_int);
        lcd_show_string(5, 50, 55, 12, 12, buf, BLUE);
        snprintf(buf, sizeof(buf), "Humi: %d%%", dht.humi_int);
        lcd_show_string(5, 70, 55, 12, 12, buf, BLUE);
        App_Lcd_Unlock();
    }

    /* 红外物品状态（右侧列）*/
    if (force_redraw) {
        item_state_e item = App_Sensor_GetItem();
        const char *item_str = (item == ITEM_PRESENT) ? "Item: YES" : "Item: NO";
        App_Lcd_Lock();
        lcd_show_string(185, 50, 55, 12, 12, (char *)item_str, BROWN);
        App_Lcd_Unlock();
    }

    /* 底部开柜按钮 */
    if (force_redraw) {
        App_Lcd_Lock();
        lcd_fill(BTN_OPEN_X1, BTN_OPEN_Y1, BTN_OPEN_X2, BTN_OPEN_Y2, BLUE);
        lcd_draw_rectangle(BTN_OPEN_X1, BTN_OPEN_Y1, BTN_OPEN_X2, BTN_OPEN_Y2, BLACK);
        lcd_show_string(70, 265, 100, 16, 16, (char *)"OPEN", WHITE);
        App_Lcd_Unlock();
    }
}

/* ============================================================
 *  检测触摸坐标是否在开柜按钮内
 * ============================================================ */
static uint8_t IsTouchInOpenBtn(uint16_t x, uint16_t y)
{
    return (x >= BTN_OPEN_X1 && x <= BTN_OPEN_X2 &&
            y >= BTN_OPEN_Y1 && y <= BTN_OPEN_Y2) ? 1U : 0U;
}

/* ============================================================
 *  初始化 UI
 * ============================================================ */
void LcdUI_Init(void)
{
    s_current_page = PAGE_MAIN;
    s_last_drawn_state = (cabinet_state_e)0xFF;
    s_last_temp_tick = 0;
    uart_printf_mutex("[LCD-UI] Init. Page=MAIN\r\n");
}

void LcdUI_SetPage(ui_page_e page)
{
    if (page < PAGE_MAX) {
        s_current_page = page;
        s_last_drawn_state = (cabinet_state_e)0xFF;  /* 强制重绘 */
        uart_printf_mutex("[LCD-UI] Page switch -> %d\r\n", page);
    }
}

ui_page_e LcdUI_GetPage(void)
{
    return s_current_page;
}

/* ============================================================
 *  TaskLcdUI：UI 主任务
 *  - 20ms 轮询触摸 + 状态刷新
 *  - 状态变化时重绘对应区域
 *  - 温湿度 2 秒刷新一次
 * ============================================================ */
void TaskLcdUI(void *argument)
{
    (void)argument;
    uint8_t last_touch_state = 0;

    /* 初始化第一帧（force_redraw=1）*/
    cabinet_state_e state = Cabinet_FSM_GetState();
    DrawPageMain(state, 1);

    uart_printf_mutex("[LCD-UI] TaskLcdUI start.\r\n");

    for (;;) {
        /* 1. 扫描触摸 */
        uint8_t pressed = tp_scan(0);
        if (pressed && last_touch_state == 0) {
            /* 按下沿：判断是否在开柜按钮内 */
            uint16_t tx = tp_dev.x;
            uint16_t ty = tp_dev.y;
            uart_printf_mutex("[LCD-UI] Touch: (%u,%u)\r\n", tx, ty);

            if (s_current_page == PAGE_MAIN && IsTouchInOpenBtn(tx, ty)) {
                uart_printf_mutex("[LCD-UI] OPEN button pressed!\r\n");
                Cabinet_FSM_OpenRequest();
            }
        }
        last_touch_state = pressed;

        /* 2. 状态刷新（只在状态变化时重绘状态矩形）*/
        state = Cabinet_FSM_GetState();
        DrawPageMain(state, 0);   /* force_redraw=0，只刷变化的区域 */

        osDelay(20);   /* 20ms 扫描 = 50Hz */
    }
}

