#ifndef __LCD_UI_H
#define __LCD_UI_H

#include "main.h"
#include "cmsis_os2.h"

/* ============================================================
 *  D9: LCD 触屏 UI 框架（替代 Qt 上位机 4 个 Tab）
 *
 *  页面布局：
 *    PAGE_MAIN    = 主页面（柜态监控 + 手动开柜按钮）
 *    PAGE_MQTT    = MQTT 控制台（D10 实现）
 *    PAGE_OTA     = OTA 升级（D11 实现）
 *    PAGE_SETTING = 系统设置（D12 实现）
 *
 *  D9 只实现 PAGE_MAIN，其他页面 D10-D12 实现
 * ============================================================ */

typedef enum {
    PAGE_MAIN = 0,      /* 主页面：柜态监控 */
    PAGE_MQTT,          /* MQTT 控制台 */
    PAGE_OTA,          /* OTA 升级 */
    PAGE_SETTING,       /* 系统设置 */
    PAGE_MAX           /* 页面总数 */
} ui_page_e;

/* ---------- 任务参数 ---------- */
#define TASK_LCD_UI_STACK_SIZE_BYTES    (1024U * 4U)   /* 4096 Bytes = 1024 Words */
#define TASK_LCD_UI_PRIORITY            (osPriorityNormal)   /* 24，和 TaskLcdTest 一致 */

/* ---------- API ---------- */
void LcdUI_Init(void);                   /* 初始化 UI（画第一帧）*/
void LcdUI_SetPage(ui_page_e page);      /* 切换页面 */
ui_page_e LcdUI_GetPage(void);           /* 获取当前页面 */
void TaskLcdUI(void *argument);          /* UI 主任务 */

/* D11: MQTT 收到 OTA 命令后调用，缓存新版本信息供 OTA 页面显示 */
void LcdUI_SetOtaCommand(uint16_t major, uint16_t minor,
                         uint16_t patch, uint16_t build,
                         uint32_t size, uint32_t crc);

#endif /* __LCD_UI_H */

