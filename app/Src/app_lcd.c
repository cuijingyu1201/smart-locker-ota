/**
 * @file    app_lcd.c
 * @brief   D17 �������LCD ��ʼ����װ + FSMC ������ + TaskLcdTest ��С��֤
 *          �����ע�⣺����ȫ�� static / BSS �Σ���ջ�������ĿӲԼ����
 */
#include "app_lcd.h"
#include "app_uart.h"     /* ���� uart_printf_mutex ��ӡ������־ */
#include <stdio.h>

/* ============================================================
 * ȫ�ֻ�����������壨�� app_ipc.c ������������ͬ���
 * ============================================================ */
osMutexId_t g_lcd_mutex_handle = NULL;

/* ============================================================
 * LCD ������ + IPC ��ʼ��
 *  ���� App_IPC_Init �е��ã���֤ App_Lcd_Lock() �����񴴽�ǰ�Ϳ��ã�
 * ============================================================ */
void App_Lcd_IPC_Init(void)
{
    g_lcd_mutex_handle = osMutexNew(NULL);
    if (g_lcd_mutex_handle == NULL) {
        uart_printf_mutex("[LCD] Mutex create FAIL! (������д�����ޱ���)\r\n");
    } else {
        uart_printf_mutex("[LCD] Mutex created OK.\r\n");
    }
}

/* ============================================================
 * LCD FSMC ������/��������װ������������ã�������������
 * ============================================================ */
void App_Lcd_Lock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;   /* ��������û������������ǰ����ʱ����ֱ�ӹ� */
    }
    /* 200ms ��ʱ�ò������ͷ�������ֹ��������ֱ�� osOK ��������Ŀ D16 �ӹ�˼· */
    (void)osMutexAcquire(g_lcd_mutex_handle, 200U);
}

void App_Lcd_Unlock(void)
{
    if (g_lcd_mutex_handle == NULL) {
        return;
    }
    /* D16 �ӹ̣�ֻ�гɹ� Acquire ���� Release����ֹ"δ��ȡ���ͷ�" */
    (void)osMutexRelease(g_lcd_mutex_handle);
}

/* ============================================================
 * LCD ��ʼ����װ����������֮ǰ���ã�FreeRTOS ����� FSMC ʱ��
 *  ���ܣ��������� + FSMC GPIO ʱ�ӿ� + ������ԭ�� lcd_init()���ڲ����� FSMC ʱ��+LCD IC�Ĵ�����
 * ============================================================ */
int32_t App_Lcd_Init(void)
{
    /* 1. ��ǿ�Ƶ������⣨PB0��������ʱ�ȿ�������û�������Ų��Դ/�������� */
    __HAL_RCC_GPIOB_CLK_ENABLE();                /* ȷ�� PB0 ʱ���ѿ���CubeMX��û��FSMC֮ǰҲ���ã� */
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin   = LCD_BL_GPIO_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LCD_BL_GPIO_PORT, &GPIO_InitStruct);
    LCD_BL(1);   /* ���� = �ߵ�ƽ���� */
    uart_printf_mutex("[LCD] BL(PB0)=HIGH set OK.\r\n");

    /* 2. ��ʱ 10ms �� LCD ��Դ�ͱ����ϵ��ȶ���HAL_Delay ���� SysTick��CubeMX ����ʱ�Ѿ���� SysTick=1ms �ˣ� */
    HAL_Delay(10);
	

    /* 3. ������ԭ�� lcd_init()���ڲ��� FSMC GPIO + FSMC ������ + ��LCD ID + ��ӦIC�Ĵ������г�ʼ�� */
    lcd_init();

    /* 4. ��ʼ����ɣ���ӡ LCD ID �ͷֱ��ʣ����ڿɲ飬�����ж� lcd_init �ߵ���һ���ˣ� */
    uart_printf_mutex("[LCD] Init done: ID=0x%04X W=%u H=%u\r\n",
                      (unsigned)lcddev.id,
                      (unsigned)lcddev.width,
                      (unsigned)lcddev.height);

    return 0;
}

/* ============================================================
 * TaskLcdTest����С��ʾ��֤���񣨽�����֤�ã�D9��ɾ��ΪTaskLcdUI��
 *  ���̣���������������ܵ����� �� ��LCD �� ���װ��ֱ��� + ���� PASS + 500ms ��/��������˸
 * ============================================================ */
void TaskLcdTest(void *argument)
{
    (void)argument;

    /* ��������������������ʼҳ�� */
    App_Lcd_Lock();
    {
        lcd_clear(BLUE);   /* ������ɫ���� */
        /* �����������֣�16���֣���ɫ���� + ��ɫ״̬�� */
        lcd_show_string(10, 10, 240, 16, 16, "D17: Locker CTRL v1.0.0", WHITE);
        lcd_show_string(10, 32, 240, 16, 16, "LCD Bringup  : PASS",   GREEN);

        /* ��ӡ�ֱ�����Ϣ��12���֣���ɫС�֣� */
        char info_buf[64];
        snprintf(info_buf, sizeof(info_buf), "ID:0x%04X  %ux%u",
                 (unsigned)lcddev.id,
                 (unsigned)lcddev.width,
                 (unsigned)lcddev.height);
        lcd_show_string(10, 60, 240, 12, 12, info_buf, YELLOW);
    }
    App_Lcd_Unlock();

    uart_printf_mutex("[LCD] TaskLcdTest start: UI drawn, entering blink loop.\r\n");

    /* ��ѭ����500ms �л�һ�η�����ɫ����֤ LCD д���� FreeRTOS �����²������� */
    uint8_t blink_state = 0;
    for (;;)
    {
        blink_state ^= 1;   /* ��ת 0/1 */

        App_Lcd_Lock();
        {
            /* �� (10, 90) λ�û�һ�� 40��40 ���飬״̬0=�� ״̬1=��(�ͱ���ͬɫ=����������) */
            if (blink_state) {
                lcd_fill(10, 90, 10 + 40, 90 + 40, RED);
            } else {
                lcd_fill(10, 90, 10 + 40, 90 + 40, BLUE);
            }
        }
        App_Lcd_Unlock();

        osDelay(500);   /* FreeRTOS ������ʱ���ͷ� CPU ���������� */
    }
}