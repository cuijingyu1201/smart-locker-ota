#include <stdio.h>
#include <string.h>
#include "lcd_ui.h"
#include "app_lcd.h"        /* App_Lcd_Lock/Unlock */
#include "app_uart.h"
#include "lcd.h"            /* lcd_clear/fill/string/rectangle, g_back_color */
#include "touch.h"          /* tp_scan/tp_dev */
#include "cabinet_fsm.h"    /* Cabinet_FSM_GetState/OpenRequest */
#include "app_dht11.h"      /* g_dht11_data/mutex */
#include "app_sensor.h"     /* App_Sensor_GetItem */
#include "flash_param.h"
#include "code_check.h"     /*  soft keypad -> code input */
#include "ota_manager.h"    /*  OTA_TriggerUpgrade, FW_VER_* */


/* ============================================================
 *  static state (BSS, no stack pressure)
 * ============================================================ */
static ui_page_e s_current_page = PAGE_MAIN;  
static cabinet_state_e s_last_drawn_state = (cabinet_state_e)0xFF;
static uint32_t s_last_temp_tick = 0;
static uint8_t  s_mqtt_page_drawn = 0;
static uint8_t  s_main_page_drawn = 0;
static item_state_e s_last_item_state = (item_state_e)0xFF;  /* item box redraw cache */
static uint8_t  s_ota_page_drawn = 0;                        /* OTA page redraw cache */
static uint8_t  s_setting_page_drawn = 0;                    /* Setting page redraw cache */

/* ===== PAGE_OTA: OTA upgrade state ===== */
static uint16_t s_ota_new_major = 0;
static uint16_t s_ota_new_minor = 0;
static uint16_t s_ota_new_patch = 0;
static uint16_t s_ota_new_build = 0;
static uint32_t s_ota_new_size = 0;
static uint32_t s_ota_new_crc = 0;
static uint8_t  s_ota_cmd_received = 0;    /* 1=收到 MQTT OTA 命令 */
static uint8_t  s_ota_pending = 0;         /* 1=有更新待处理，主界面 OTA 按钮变红 */
static uint8_t  s_ota_last_result = 0xFF;  /* 0=成功 1=CRC失败 2=写Flash失败 3=超时, 0xFF=无升级 */
static uint8_t  s_ota_btn_red = 0;         /* OTA 按钮当前是否红色（重绘缓存） */
static uint8_t  s_ota_result_ack = 0;   /* 1=本次开机已展示过升级结果，之后显示 Ready */


/* MQTT page redraw cache: only refresh regions whose content changed,
 * avoids flicker and wasted FSMC bandwidth at the 20ms refresh rate */
static uint8_t  s_code_drawn_len = 0xFF;
static char     s_code_drawn[CODE_LEN + 1];
static uint8_t  s_cursor_on = 0;
static uint16_t s_cursor_x = 0;

/* status bar cache */
static uint8_t  s_bar_kind = 0xFF;   /* 0 LOCKED / 1 UNLOCKED / 2 FAULT / 3 CODE-LOCK */
static uint32_t s_bar_secs = 0xFFFFFFFFU;

/* ===== PAGE_MAIN: bottom 3 nav buttons =====
 * 3 buttons * 66px wide + 2 * 12px gap = 222px
 * left/right margin = (240-222)/2 = 9px
 */
#define BTN_Y1          250
#define BTN_Y2          300
#define BTN_H           50

#define BTN_PICKUP_X1   9
#define BTN_PICKUP_X2   75

#define BTN_OTA_X1      87
#define BTN_OTA_X2      153

#define BTN_SETTING_X1  165
#define BTN_SETTING_X2  231

/* ===== PAGE_OTA: START / ROLLBACK buttons ===== */
#define OTA_BTN_START_X1    20
#define OTA_BTN_START_X2    110
#define OTA_BTN_ROLLBACK_X1 130
#define OTA_BTN_ROLLBACK_X2 220
/* Y 坐标复用主界面的 BTN_Y1/BTN_Y2 = 250/300 */


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
 *  PAGE 1: main status page (Smart-Locker layout)
 *  Layout (240x320):
 *    y5   title "Smart-Locker"
 *    y28  separator
 *    y35  "Temp" label / y55  temp value (red 16px)
 *    y35  "Humi" label / y55  humi value (blue 16px)
 *    y80  dashed separator
 *    y90  "Cabinet Item" label
 *    y110 item color box (200x50) green=YES / gray=NO
 *    y250 three nav buttons: [Pickup][OTA][Setting]
 * ============================================================ */
static void DrawPageMain(uint8_t force_redraw, cabinet_state_e state)
{
    (void)state;   /* main page no longer shows cabinet state rectangle */

    if (force_redraw) {
        App_Lcd_Lock();
        lcd_clear(WHITE);

        /* title: "Smart-Locker" 12 chars * 8px = 96, center = (240-96)/2 = 72 */
        Ui_TextOnBg(72, 5, 16, "Smart-Locker", BLACK, WHITE);

        /* separator line */
        lcd_draw_hline(20, 28, 200, GRAY);

        /* temp/humi labels */
        Ui_TextOnBg(30, 35, 12, "Temp", GRAY, WHITE);
        Ui_TextOnBg(150, 35, 12, "Humi", GRAY, WHITE);

        /* dashed separator */
        lcd_draw_hline(20, 90, 200, LGRAY);

        /* item label */
        Ui_TextOnBg(20, 100, 12, "Cabinet Item", GRAY, WHITE);

        App_Lcd_Unlock();
    }

    /* ---- temp/humi values: refresh every 2s ---- */
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
        char buf[16];
        App_Lcd_Lock();
        snprintf(buf, sizeof(buf), "%dC", dht.temp_int);
        Ui_TextOnBg(30, 55, 16, buf, RED, WHITE);
        snprintf(buf, sizeof(buf), "%d%%", dht.humi_int);
        Ui_TextOnBg(150, 55, 16, buf, BLUE, WHITE);
        App_Lcd_Unlock();
    }

    /* ---- item status color box: redraw only when state changes ---- */
    item_state_e item = App_Sensor_GetItem();
    if (force_redraw || item != s_last_item_state) {
        uint16_t box_color = (item == ITEM_PRESENT) ? GREEN : GRAY;
        const char *item_str = (item == ITEM_PRESENT) ? "ITEM: YES" : "ITEM: NO";

        App_Lcd_Lock();
        lcd_fill(20, 110, 220, 160, box_color);
        lcd_draw_rectangle(20, 100, 220, 160, BLACK);
        Ui_TextOnBg(84, 127, 16, item_str, WHITE, box_color);
        App_Lcd_Unlock();
        s_last_item_state = item;
    }

    /* ---- bottom 3 nav buttons ---- */
    if (force_redraw) {
        App_Lcd_Lock();
        /* Pickup button (blue bg, black text) */
        lcd_fill(BTN_PICKUP_X1, BTN_Y1, BTN_PICKUP_X2, BTN_Y2, BLUE);
        lcd_draw_rectangle(BTN_PICKUP_X1, BTN_Y1, BTN_PICKUP_X2, BTN_Y2, BLACK);
        Ui_TextOnBg(18, 267, 16, "Pickup", BLACK, BLUE);

        /* Setting button (brown bg, white text) */
        lcd_fill(BTN_SETTING_X1, BTN_Y1, BTN_SETTING_X2, BTN_Y2, BROWN);
        lcd_draw_rectangle(BTN_SETTING_X1, BTN_Y1, BTN_SETTING_X2, BTN_Y2, BLACK);
        Ui_TextOnBg(170, 267, 16, "Setting", WHITE, BROWN);

        App_Lcd_Unlock();
    }

    /* OTA button: red if update pending, brown otherwise
     * 独立检测，MQTT 收到命令后 20ms 内变红，不用等 force_redraw */
    if (force_redraw || s_ota_btn_red != s_ota_pending) {
        uint16_t ota_bg = (s_ota_pending) ? RED : BROWN;
        App_Lcd_Lock();
        lcd_fill(BTN_OTA_X1, BTN_Y1, BTN_OTA_X2, BTN_Y2, ota_bg);
        lcd_draw_rectangle(BTN_OTA_X1, BTN_Y1, BTN_OTA_X2, BTN_Y2, BLACK);
        Ui_TextOnBg(108, 267, 16, "OTA", WHITE, ota_bg);
        App_Lcd_Unlock();
        s_ota_btn_red = s_ota_pending;
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
        Ui_TextOnBg(12, 5, 16, "Pickup", BLACK, WHITE);
        /* Back button (top-right) */
        lcd_fill(180, 5, 230, 25, BLUE);
        lcd_draw_rectangle(180, 5, 230, 25, BLACK);
        Ui_TextOnBg(193, 9, 12, "Back", WHITE, BLUE);
			
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
 *  PAGE 3: OTA upgrade page
 *  Layout (240x320):
 *    y5   title "OTA Upgrade" + Back button (top-right)
 *    y28  separator
 *    y35  "Current: 1.0.0.4" (blue)
 *    y55  "New: 1.0.0.5" (red, if received)
 *    y75  "Size: 33336 bytes" (gray)
 *    y90  "CRC: 0x34518D29" (gray)
 *    y180 status text (gray=Ready / green=SUCCESS / red=FAIL)
 *    y250 [START] [ROLLBACK] buttons
 * ============================================================ */
static void DrawPageOta(uint8_t force_redraw)
{
    if (force_redraw) {
        App_Lcd_Lock();
        lcd_clear(WHITE);

        /* title */
        Ui_TextOnBg(72, 5, 16, "OTA Upgrade", BLACK, WHITE);

        /* Back button (top-right, same as Pickup page) */
        lcd_fill(180, 5, 230, 25, BLUE);
        lcd_draw_rectangle(180, 5, 230, 25, BLACK);
        Ui_TextOnBg(193, 9, 12, "Back", WHITE, BLUE);

        /* separator */
        lcd_draw_hline(20, 28, 200, GRAY);

        /* current version */
        char buf[32];
        snprintf(buf, sizeof(buf), "Current: %u.%u.%u.%u",
                 FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH, FW_BUILD_NUM);
        Ui_TextOnBg(10, 35, 16, buf, BLUE, WHITE);

        /* new version info (if received) */
        if (s_ota_cmd_received) {
            snprintf(buf, sizeof(buf), "New: %u.%u.%u.%u",
                     s_ota_new_major, s_ota_new_minor,
                     s_ota_new_patch, s_ota_new_build);
            Ui_TextOnBg(10, 55, 16, buf, RED, WHITE);
            snprintf(buf, sizeof(buf), "Size: %lu bytes",
                     (unsigned long)s_ota_new_size);
            Ui_TextOnBg(10, 75, 12, buf, GRAY, WHITE);
            snprintf(buf, sizeof(buf), "CRC: 0x%08lX",
                     (unsigned long)s_ota_new_crc);
            Ui_TextOnBg(10, 90, 12, buf, GRAY, WHITE);
        } else {
            Ui_TextOnBg(10, 55, 12, "No OTA command received", GRAY, WHITE);
        }

        /* status text: 升级结果只在开机后首次进入 OTA 页面时显示一次，
         * 之后显示 Ready，避免一直显示 SUCCESS */
        const char *status_str;
        uint16_t status_color;
        if (!s_ota_result_ack && s_ota_last_result != 0xFF) {
            if (s_ota_last_result == 0) {
                status_str = "SUCCESS";
                status_color = GREEN;
            } else {
                status_str = "FAIL";
                status_color = RED;
            }
            s_ota_result_ack = 1;   /* 已展示，下次显示 Ready */
        } else {
            status_str = "Ready";
            status_color = GRAY;
        }
        Ui_TextOnBg(10, 180, 16, status_str, status_color, WHITE);

        /* START button (blue bg, white text) */
        lcd_fill(OTA_BTN_START_X1, BTN_Y1, OTA_BTN_START_X2, BTN_Y2, BLUE);
        lcd_draw_rectangle(OTA_BTN_START_X1, BTN_Y1, OTA_BTN_START_X2, BTN_Y2, BLACK);
        /* "START" 5 chars * 8 = 40, center in 90 -> x = 20 + (90-40)/2 = 45 */
        Ui_TextOnBg(45, 267, 16, "START", WHITE, BLUE);

        /* ROLLBACK button (brown bg, white text) */
        lcd_fill(OTA_BTN_ROLLBACK_X1, BTN_Y1, OTA_BTN_ROLLBACK_X2, BTN_Y2, BROWN);
        lcd_draw_rectangle(OTA_BTN_ROLLBACK_X1, BTN_Y1, OTA_BTN_ROLLBACK_X2, BTN_Y2, BLACK);
        /* "ROLLBACK" 8 chars * 8 = 64, center in 90 -> x = 130 + (90-64)/2 = 143 */
        Ui_TextOnBg(143, 267, 16, "ROLLBACK", WHITE, BROWN);

        App_Lcd_Unlock();
    }
}

/* ============================================================
 *  PAGE 4: System settings (placeholder, D12 will implement)
 * ============================================================ */
static void DrawPageSetting(uint8_t force_redraw)
{
    if (force_redraw) {
        App_Lcd_Lock();
        lcd_clear(WHITE);
        Ui_TextOnBg(68, 5, 16, "Settings", BLACK, WHITE);
        lcd_draw_hline(20, 28, 200, GRAY);
        Ui_TextOnBg(60, 140, 16, "Coming soon...", GRAY, WHITE);

        /* back button */
        lcd_fill(BTN_PICKUP_X1, BTN_Y1, BTN_PICKUP_X2, BTN_Y2, BLUE);
        lcd_draw_rectangle(BTN_PICKUP_X1, BTN_Y1, BTN_PICKUP_X2, BTN_Y2, BLACK);
        Ui_TextOnBg(27, 267, 16, "Back", BLACK, BLUE);
        App_Lcd_Unlock();
    }
}

/* ============================================================
 *  touch hit tests
 * ============================================================ */
static uint8_t IsTouchInPickupBtn(uint16_t x, uint16_t y)
{
    return (x >= BTN_PICKUP_X1 && x <= BTN_PICKUP_X2 &&
            y >= BTN_Y1 && y <= BTN_Y2) ? 1U : 0U;
}

static uint8_t IsTouchInOtaBtn(uint16_t x, uint16_t y)
{
    return (x >= BTN_OTA_X1 && x <= BTN_OTA_X2 &&
            y >= BTN_Y1 && y <= BTN_Y2) ? 1U : 0U;
}

static uint8_t IsTouchInSettingBtn(uint16_t x, uint16_t y)
{
    return (x >= BTN_SETTING_X1 && x <= BTN_SETTING_X2 &&
            y >= BTN_Y1 && y <= BTN_Y2) ? 1U : 0U;
}

/* ===== PAGE_OTA: START / ROLLBACK button hit tests ===== */
static uint8_t IsTouchInOtaStartBtn(uint16_t x, uint16_t y)
{
    return (x >= OTA_BTN_START_X1 && x <= OTA_BTN_START_X2 &&
            y >= BTN_Y1 && y <= BTN_Y2) ? 1U : 0U;
}

static uint8_t IsTouchInOtaRollbackBtn(uint16_t x, uint16_t y)
{
    return (x >= OTA_BTN_ROLLBACK_X1 && x <= OTA_BTN_ROLLBACK_X2 &&
            y >= BTN_Y1 && y <= BTN_Y2) ? 1U : 0U;
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
    s_current_page = PAGE_MAIN;
    s_last_drawn_state = (cabinet_state_e)0xFF;
    s_last_temp_tick = 0;
    s_mqtt_page_drawn = 0;
    s_main_page_drawn = 0;
    s_ota_page_drawn = 0;
    s_setting_page_drawn = 0;

    /* D11: 读上次 OTA 结果，供 OTA 页面显示 */
    flash_param_t param;
    if (FlashParam_Load(&param) == 0) {
        s_ota_last_result = (uint8_t)param.last_ota_result;
    }

    uart_printf_mutex("[LCD-UI] Init. Page=MAIN\r\n");
}

void LcdUI_SetPage(ui_page_e page)
{
    if (page < PAGE_MAX) {
        s_current_page = page;
        s_last_drawn_state = (cabinet_state_e)0xFF;
        s_mqtt_page_drawn = 0;
        s_main_page_drawn = 0;
        s_ota_page_drawn = 0;
        s_setting_page_drawn = 0;
        uart_printf_mutex("[LCD-UI] Page switch -> %d\r\n", page);
    }
}

ui_page_e LcdUI_GetPage(void)
{
    return s_current_page;
}

void LcdUI_SetOtaCommand(uint16_t major, uint16_t minor,
                         uint16_t patch, uint16_t build,
                         uint32_t size, uint32_t crc)
{
    s_ota_new_major = major;
    s_ota_new_minor = minor;
    s_ota_new_patch = patch;
    s_ota_new_build = build;
    s_ota_new_size  = size;
    s_ota_new_crc   = crc;
    s_ota_cmd_received = 1;
  	s_ota_pending = 1;    /* 主界面 OTA 按钮变红提示 */
    uart_printf_mutex("[LCD-UI] OTA command cached: v%u.%u.%u.%u size=%lu\r\n",
                     major, minor, patch, build, (unsigned long)size);
}

/* ============================================================
 *  TaskLcdUI: 20ms touch scan + page refresh
 * ============================================================ */
void TaskLcdUI(void *argument)
{
    (void)argument;
    uint8_t last_touch_state = 0;
    cabinet_state_e state;

    /* first frame: MAIN page */
    DrawPageMain(1, Cabinet_FSM_GetState());
    s_main_page_drawn = 1;

    uart_printf_mutex("[LCD-UI] TaskLcdUI start.\r\n");

    for (;;) {
        uint8_t pressed = tp_scan(0);

        if (pressed && last_touch_state == 0) {
            uint16_t tx = tp_dev.x;
            uint16_t ty = tp_dev.y;
            uart_printf_mutex("[LCD-UI] Touch: (%u,%u)\r\n", tx, ty);

            if (s_current_page == PAGE_MAIN) {
								if (IsTouchInPickupBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] Pickup button -> PAGE_MQTT\r\n");
										LcdUI_SetPage(PAGE_MQTT);
								}
								else if (IsTouchInOtaBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] OTA button -> PAGE_OTA\r\n");
										LcdUI_SetPage(PAGE_OTA);
								}
								else if (IsTouchInSettingBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] Setting button -> PAGE_SETTING\r\n");
										LcdUI_SetPage(PAGE_SETTING);
								}
						}
            else if (s_current_page == PAGE_MQTT) {
                /* Back button: x=180~230, y=5~25 */
                if (tx >= 180 && tx <= 230 && ty >= 5 && ty <= 25) {
                    uart_printf_mutex("[LCD-UI] Back button -> PAGE_MAIN\r\n");
                    LcdUI_SetPage(PAGE_MAIN);
                }
                else {
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
						else if (s_current_page == PAGE_OTA) {
								/* Back button (top-right, same as Pickup page) */
								if (tx >= 180 && tx <= 230 && ty >= 5 && ty <= 25) {
										uart_printf_mutex("[LCD-UI] Back button -> PAGE_MAIN\r\n");
										LcdUI_SetPage(PAGE_MAIN);
								}
								/* START button */
								else if (IsTouchInOtaStartBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] OTA START button pressed!\r\n");
										if (s_ota_cmd_received) {
												uart_printf_mutex("[LCD-UI] Triggering OTA upgrade...\r\n");
											  s_ota_pending = 0;         
				                s_ota_result_ack = 1;
												OTA_TriggerUpgrade(s_ota_new_major, s_ota_new_minor,
																					s_ota_new_patch, s_ota_new_build,
																					s_ota_new_crc, s_ota_new_size, 0);
										} else {
												uart_printf_mutex("[LCD-UI] No OTA command, cannot start\r\n");
										}
								}
								/* ROLLBACK button */
								else if (IsTouchInOtaRollbackBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] OTA ROLLBACK button pressed!\r\n");
										if (s_ota_cmd_received) {
											  s_ota_pending = 0;          /* 开始升级，清除红色提示 */
                        s_ota_result_ack = 1;       /* 本次结果已处理 */
												/* force=1 允许降级/重装 */
												OTA_TriggerUpgrade(s_ota_new_major, s_ota_new_minor,
																					s_ota_new_patch, s_ota_new_build,
																					s_ota_new_crc, s_ota_new_size, 1);
										}
								}
						}
						else if (s_current_page == PAGE_SETTING) {
								/* Back button = same area as Pickup button */
								if (IsTouchInPickupBtn(tx, ty)) {
										uart_printf_mutex("[LCD-UI] Back button -> PAGE_MAIN\r\n");
										LcdUI_SetPage(PAGE_MAIN);
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
        else if (s_current_page == PAGE_OTA) {
            DrawPageOta(s_ota_page_drawn == 0U);
            s_ota_page_drawn = 1;
        }
        else if (s_current_page == PAGE_SETTING) {
            DrawPageSetting(s_setting_page_drawn == 0U);
            s_setting_page_drawn = 1;
        }

        CodeCheck_BeepPoll();   /* non-blocking buzzer turn-off check */
        osDelay(20);
    }
}
