#include <stdio.h>
#include <string.h>
#include "lcd_ui.h"
#include "app_lcd.h"       /* App_Lcd_Lock/Unlock */
#include "app_uart.h"
#include "lcd.h"            /* lcd_clear/fill/string/rectangle, g_back_color */
#include "touch.h"          /* tp_scan/tp_dev */
#include "cabinet_fsm.h"    /* Cabinet_FSM_GetState/OpenRequest */
#include "app_dht11.h"      /* g_dht11_data/mutex */
#include "app_sensor.h"     /* App_Sensor_GetItem */
#include "flash_param.h"
#include "code_check.h"     /* D10: soft keypad -> code input */

/* ============================================================
 *  static state (BSS, no stack pressure)
 * ============================================================ */
static ui_page_e s_current_page = PAGE_MQTT;  /* D10 test: MQTT default, revert later */
static cabinet_state_e s_last_drawn_state = (cabinet_state_e)0xFF;
static uint32_t s_last_temp_tick = 0;
static uint8_t  s_mqtt_page_drawn = 0;
static uint8_t  s_main_page_drawn = 0;

/* MQTT page redraw cache: only refresh regions whose content changed,
 * avoids flicker and wasted FSMC bandwidth at the 20ms refresh rate */
static uint8_t  s_code_drawn_len = 0xFF;
static char     s_code_drawn[CODE_LEN + 1];
static uint8_t  s_cursor_on = 0;
static uint16_t s_cursor_x = 0;

/* status bar cache */
static uint8_t  s_bar_kind = 0xFF;   /* 0 LOCKED / 1 UNLOCKED / 2 FAULT / 3 CODE-LOCK */
static uint32_t s_bar_secs = 0xFFFFFFFFU;

/* ===== PAGE_MAIN: OPEN button ===== */
#define BTN_OPEN_X1    40
#define BTN_OPEN_X2    200
#define BTN_OPEN_Y1    250
#define BTN_OPEN_Y2    300

/* ===== PAGE_MQTT: enlarged 3x4 keypad =====
 * Screen 240x320 vertical budget:
 *   title y5 / divider y25 / code box y29..61 / status bar y64..82
 *   keypad y86: key 52 high, gap 8 -> 86 / 146 / 206 / 266 (bottom 317)
 */
#define KP_X1          12
#define KP_Y1          86
#define KP_KEY_W       66
#define KP_KEY_H       52
#define KP_GAP_X       9
#define KP_GAP_Y       8

/* code display box: 32px high so 24-size glyphs (y33..56) and the
 * cursor bar (y58..60) never overlap */
#define CODE_BOX_X1    66
#define CODE_BOX_Y1    29
#define CODE_BOX_X2    198
#define CODE_BOX_Y2    61
#define CODE_DIGIT_X   76
#define CODE_DIGIT_DX  20
#define CODE_DIGIT_Y   33
#define CODE_CURSOR_Y  58

/* status bar (shows lock / code-lock / fault state) */
#define STATUS_X1      10
#define STATUS_Y1      64
#define STATUS_X2      230
#define STATUS_Y2      82

/* ============================================================
 *  text helper: lcd_show_char(mode=0) fills every non-glyph pixel
 *  with g_back_color, so text on a colored background must temporarily
 *  set g_back_color to that background color (caller MUST hold LCD lock)
 * ============================================================ */
static void Ui_TextOnBg(uint16_t x, uint16_t y, uint8_t size,
                        const char *p, uint16_t fg, uint16_t bg)
{
    uint32_t saved = g_back_color;
    g_back_color = bg;
    /* ASCII glyph cell: width = strlen * size/2, one row only */
    lcd_show_string(x, y, (uint16_t)(strlen(p) * (size / 2U)), size,
                    size, (char *)p, fg);
    g_back_color = saved;
}

/* ============================================================
 *  PAGE 1: main status page
 * ============================================================ */
static void DrawPageMain(uint8_t force_redraw, cabinet_state_e state)
{
    if (force_redraw) {
        App_Lcd_Lock();
        lcd_clear(WHITE);
        Ui_TextOnBg(10, 5, 16, "Locker CTRL v1.0.0", BLACK, WHITE);
        Ui_TextOnBg(10, 25, 12, "Status Monitor", GRAY, WHITE);
        App_Lcd_Unlock();
    }

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
        lcd_fill(60, 45, 180, 165, box_color);
        lcd_draw_rectangle(60, 45, 180, 165, BLACK);
        /* 16-size glyph is 8px wide; center 6-char word in 120px box */
        Ui_TextOnBg(80, 95, 16, state_str, WHITE, box_color);
        App_Lcd_Unlock();
        s_last_drawn_state = state;
    }

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
        Ui_TextOnBg(5, 50, 12, buf, BLUE, WHITE);
        snprintf(buf, sizeof(buf), "Humi: %d%%", dht.humi_int);
        Ui_TextOnBg(5, 70, 12, buf, BLUE, WHITE);
        App_Lcd_Unlock();
    }

    if (force_redraw) {
        item_state_e item = App_Sensor_GetItem();
        const char *item_str = (item == ITEM_PRESENT) ? "Item: YES" : "Item: NO";
        App_Lcd_Lock();
        Ui_TextOnBg(185, 50, 12, item_str, BROWN, WHITE);
        App_Lcd_Unlock();
    }

    if (force_redraw) {
        App_Lcd_Lock();
        lcd_fill(BTN_OPEN_X1, BTN_OPEN_Y1, BTN_OPEN_X2, BTN_OPEN_Y2, BLUE);
        lcd_draw_rectangle(BTN_OPEN_X1, BTN_OPEN_Y1, BTN_OPEN_X2, BTN_OPEN_Y2, BLACK);
        /* center "OPEN" (32px) in 161px wide button -> x = 40 + 64 */
        Ui_TextOnBg(104, 265, 16, "OPEN", BLACK, BLUE);
        App_Lcd_Unlock();
    }
}

/* ============================================================
 *  PAGE 2: MQTT console = pickup code keypad only
 *  (MQTT traffic log stays on UART, not on LCD)
 * ============================================================ */
static void DrawKeypadKey(uint8_t key)
{
    static const char *const kp_label[12] = {
        "1","2","3",
        "4","5","6",
        "7","8","9",
        "C","0","OK"
    };
    static const uint16_t kp_bg[12] = {
        BLUE, BLUE, BLUE,
        BLUE, BLUE, BLUE,
        BLUE, BLUE, BLUE,
        RED,  BLUE, GREEN
    };

    uint8_t r = (uint8_t)(key / 3U);
    uint8_t c = (uint8_t)(key % 3U);
    uint16_t x1 = (uint16_t)(KP_X1 + c * (KP_KEY_W + KP_GAP_X));
    uint16_t y1 = (uint16_t)(KP_Y1 + r * (KP_KEY_H + KP_GAP_Y));
    uint16_t x2 = x1 + KP_KEY_W - 1U;
    uint16_t y2 = y1 + KP_KEY_H - 1U;

    /* 24-size ASCII glyph is 12 wide / 24 high; "OK" is two glyphs */
    uint16_t text_w = (uint16_t)(strlen(kp_label[key]) * 12U);

    App_Lcd_Lock();
    lcd_fill(x1, y1, x2, y2, kp_bg[key]);
    lcd_draw_rectangle(x1, y1, x2, y2, BLACK);
    /* all keys: BLACK glyphs on blue/red/green background */
    Ui_TextOnBg((uint16_t)(x1 + (KP_KEY_W - text_w) / 2U),
                (uint16_t)(y1 + (KP_KEY_H - 24U) / 2U),
                24, kp_label[key], BLACK, kp_bg[key]);
    App_Lcd_Unlock();
}

/* status kinds */
#define BAR_LOCKED     0U   /* cabinet CLOSED, servo locked   */
#define BAR_UNLOCKED   1U   /* OPENING / WAIT_PICKUP / CLOSING */
#define BAR_FAULT      2U   /* cabinet FAULT                   */
#define BAR_CODE_LOCK  3U   /* wrong-code 30s lockout          */

static void DrawStatusBar(uint8_t kind, uint32_t secs)
{
    static const uint16_t bar_bg[4] = { GREEN, RED,   YELLOW, BROWN  };
    static const uint16_t bar_fg[4] = { WHITE, WHITE, BLACK,  WHITE  };
    static const char *const bar_lbl[4] = { "LOCKED", "UNLOCKED", "FAULT", "" };

    char buf[16];
    const char *p;
    if (kind == BAR_CODE_LOCK) {
        snprintf(buf, sizeof(buf), "LOCK %lus", (unsigned long)secs);
        p = buf;
    } else {
        p = bar_lbl[kind];
    }

    /* 16-size ASCII glyph is 8px wide, center text in the bar */
    uint16_t tw = (uint16_t)(strlen(p) * 8U);
    uint16_t tx = (uint16_t)(STATUS_X1 + ((STATUS_X2 - STATUS_X1 + 1U) - tw) / 2U);
    uint16_t ty = (uint16_t)(STATUS_Y1 + ((STATUS_Y2 - STATUS_Y1 + 1U) - 16U) / 2U);

    App_Lcd_Lock();
    lcd_fill(STATUS_X1, STATUS_Y1, STATUS_X2, STATUS_Y2, bar_bg[kind]);
    lcd_draw_rectangle(STATUS_X1, STATUS_Y1, STATUS_X2, STATUS_Y2, BLACK);
    Ui_TextOnBg(tx, ty, 16, p, bar_fg[kind], bar_bg[kind]);
    App_Lcd_Unlock();
}

static void DrawPageMqtt(uint8_t force_redraw)
{
    if (force_redraw) {
        /* invalidate redraw caches so every region repaints once */
        s_code_drawn_len = 0xFF;
        s_cursor_on = 0;
        s_bar_kind = 0xFF;
        s_bar_secs = 0xFFFFFFFFU;

        App_Lcd_Lock();
        lcd_clear(WHITE);

        /* title + divider */
        Ui_TextOnBg(12, 5, 16, "MQTT Console", BLACK, WHITE);
        lcd_draw_hline(12, 25, 216, GRAY);

        /* code label + display box */
        Ui_TextOnBg(10, 37, 16, "Code:", BLACK, WHITE);
        lcd_fill(CODE_BOX_X1, CODE_BOX_Y1, CODE_BOX_X2, CODE_BOX_Y2, LGRAY);
        lcd_draw_rectangle(CODE_BOX_X1, CODE_BOX_Y1, CODE_BOX_X2, CODE_BOX_Y2, GRAY);

        App_Lcd_Unlock();

        for (uint8_t k = 0; k < 12U; k++) {
            DrawKeypadKey(k);
        }
    }

    code_state_e cs = CodeCheck_GetState();
    cabinet_state_e cab = Cabinet_FSM_GetState();

    /* ---- status bar: code lockout takes precedence over cabinet state ---- */
    uint8_t  bar_kind;
    uint32_t bar_secs = 0;
    if (cs == CODE_STATE_LOCKED) {
        bar_kind = BAR_CODE_LOCK;
        bar_secs = CodeCheck_GetRemainLockMs() / 1000U;
    } else {
        switch (cab) {
            case CAB_STATE_CLOSED:
                bar_kind = BAR_LOCKED;
                break;
            case CAB_STATE_OPENING:
            case CAB_STATE_WAIT_PICKUP:
            case CAB_STATE_TIMEOUT_CLOSING:
                bar_kind = BAR_UNLOCKED;
                break;
            case CAB_STATE_FAULT:
            default:
                bar_kind = BAR_FAULT;
                break;
        }
    }
    if (force_redraw || bar_kind != s_bar_kind || bar_secs != s_bar_secs) {
        DrawStatusBar(bar_kind, bar_secs);
        s_bar_kind = bar_kind;
        s_bar_secs = bar_secs;
    }

    /* ---- code digits: entered = black, empty = gray '*'.
     * Repaint box interior only when content changes (no flicker). ---- */
    char code_buf[8];
    CodeCheck_GetInputBuf(code_buf, sizeof(code_buf));
    uint8_t in_len = CodeCheck_GetInputLen();

    if (force_redraw || in_len != s_code_drawn_len ||
        memcmp(code_buf, s_code_drawn, CODE_LEN) != 0) {
        App_Lcd_Lock();
        lcd_fill(CODE_BOX_X1 + 2, CODE_BOX_Y1 + 2, CODE_BOX_X2 - 2, CODE_BOX_Y2 - 2, LGRAY);
        for (uint8_t i = 0; i < CODE_LEN; i++) {
            char ch[2];
            uint16_t color;
            if (i < in_len) {
                ch[0] = code_buf[i];
                color = BLACK;
            } else {
                ch[0] = '*';
                color = GRAYBLUE;
            }
            ch[1] = '\0';
            Ui_TextOnBg((uint16_t)(CODE_DIGIT_X + i * CODE_DIGIT_DX), CODE_DIGIT_Y,
                        24, ch, color, LGRAY);
        }
        App_Lcd_Unlock();
        s_code_drawn_len = in_len;
        memcpy(s_code_drawn, code_buf, CODE_LEN);
    }

    /* blinking cursor bar under next empty digit.
     * Erase old bar before drawing at a new position. */
    uint8_t want_on = 0;
    uint16_t want_x = (uint16_t)(CODE_DIGIT_X + in_len * CODE_DIGIT_DX);
    if (cs != CODE_STATE_LOCKED && in_len < CODE_LEN) {
        want_on = ((osKernelGetTickCount() / 500U) & 1U) ? 1U : 0U;
    }
    if (want_on != s_cursor_on || (want_on && want_x != s_cursor_x)) {
        App_Lcd_Lock();
        if (s_cursor_on) {
            lcd_fill(s_cursor_x, CODE_CURSOR_Y, s_cursor_x + 12, CODE_CURSOR_Y + 2, LGRAY);
        }
        if (want_on) {
            lcd_fill(want_x, CODE_CURSOR_Y, want_x + 12, CODE_CURSOR_Y + 2, RED);
        }
        App_Lcd_Unlock();
        s_cursor_on = want_on;
        s_cursor_x = want_x;
    }
}

/* ============================================================
 *  touch hit tests
 * ============================================================ */
static uint8_t IsTouchInOpenBtn(uint16_t x, uint16_t y)
{
    return (x >= BTN_OPEN_X1 && x <= BTN_OPEN_X2 &&
            y >= BTN_OPEN_Y1 && y <= BTN_OPEN_Y2) ? 1U : 0U;
}

/* return 0..11 keypad key, 0xFF = outside keypad */
static uint8_t GetKeypadKey(uint16_t x, uint16_t y)
{
    for (uint8_t r = 0; r < 4U; r++) {
        uint16_t y1 = (uint16_t)(KP_Y1 + r * (KP_KEY_H + KP_GAP_Y));
        if (y < y1 || y > y1 + KP_KEY_H - 1U) continue;
        for (uint8_t c = 0; c < 3U; c++) {
            uint16_t x1 = (uint16_t)(KP_X1 + c * (KP_KEY_W + KP_GAP_X));
            if (x >= x1 && x <= x1 + KP_KEY_W - 1U) {
                return (uint8_t)(r * 3U + c);
            }
        }
    }
    return 0xFFU;
}

/* ============================================================
 *  public API
 * ============================================================ */
void LcdUI_Init(void)
{
    s_current_page = PAGE_MQTT;   /* D10 test: default MQTT page, revert later */
    s_last_drawn_state = (cabinet_state_e)0xFF;
    s_last_temp_tick = 0;
    s_mqtt_page_drawn = 0;
    s_main_page_drawn = 0;
    uart_printf_mutex("[LCD-UI] Init. Page=MQTT (D10 test)\r\n");
}

void LcdUI_SetPage(ui_page_e page)
{
    if (page < PAGE_MAX) {
        s_current_page = page;
        s_last_drawn_state = (cabinet_state_e)0xFF;
        s_mqtt_page_drawn = 0;
        s_main_page_drawn = 0;
        uart_printf_mutex("[LCD-UI] Page switch -> %d\r\n", page);
    }
}

ui_page_e LcdUI_GetPage(void)
{
    return s_current_page;
}

/* ============================================================
 *  TaskLcdUI: 20ms touch scan + page refresh
 * ============================================================ */
void TaskLcdUI(void *argument)
{
    (void)argument;
    uint8_t last_touch_state = 0;
    cabinet_state_e state;

    /* first frame: MQTT page (D10 test default) */
    DrawPageMqtt(1);
    s_mqtt_page_drawn = 1;

    uart_printf_mutex("[LCD-UI] TaskLcdUI start.\r\n");

    for (;;) {
        uint8_t pressed = tp_scan(0);

        if (pressed && last_touch_state == 0) {
            uint16_t tx = tp_dev.x;
            uint16_t ty = tp_dev.y;
            uart_printf_mutex("[LCD-UI] Touch: (%u,%u)\r\n", tx, ty);

            if (s_current_page == PAGE_MAIN) {
                if (IsTouchInOpenBtn(tx, ty)) {
                    uart_printf_mutex("[LCD-UI] OPEN button pressed!\r\n");
                    Cabinet_FSM_OpenRequest();
                }
            }
            else if (s_current_page == PAGE_MQTT) {
                uint8_t key = GetKeypadKey(tx, ty);
                if (key != 0xFFU) {
                    uart_printf_mutex("[LCD-UI] Keypad key=%u\r\n", key);
                    if (key <= 8U) {
                        CodeCheck_OnDigit((uint8_t)(key + 1U));   /* keys 0..8 -> digits 1..9 */
                    } else if (key == 9U) {
                        CodeCheck_OnClear();                      /* C */
                    } else if (key == 10U) {
                        CodeCheck_OnDigit(0U);                    /* 0 */
                    } else if (key == 11U) {
                        CodeCheck_OnConfirm();                    /* OK */
                    }
                }
            }
        }
        last_touch_state = pressed;

        if (s_current_page == PAGE_MAIN) {
            state = Cabinet_FSM_GetState();
            DrawPageMain(s_main_page_drawn == 0U, state);
            s_main_page_drawn = 1;
        }
        else if (s_current_page == PAGE_MQTT) {
            DrawPageMqtt(s_mqtt_page_drawn == 0U);
            s_mqtt_page_drawn = 1;
        }

        CodeCheck_BeepPoll();   /* non-blocking buzzer turn-off check */
        osDelay(20);
    }
}
