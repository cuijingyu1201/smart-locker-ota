#ifndef __CODE_CHECK_H
#define __CODE_CHECK_H

#include "main.h"
#include "cmsis_os2.h"

/* ============================================================
 *  D8: 6 λȡ����������У�飨���ذ��� + D10 LCD �����̣�
 *
 *    KEY0 ���� = �л���һλ��0->1->2->3->4->5->0 ѭ����
 *    KEY1 ���� = ��ǰλ���� +1��0->9 ѭ����
 *    KEY_WKUP  = ȷ�ϼ���У�� 6 λ����
 *
 *  У�����
 *    ��ȷ -> ���Ŷ��� + �������� + ������ + ����ʾ�� 100ms
 *    ���� -> ������ + ���� 1 �� + ������� +1
 *    10 �δ��� -> ���� 30 �룬�ڼ����а���������Ч
 * ============================================================ */

#define CODE_LEN           6U          /* 6 λȡ���� */
#define CODE_MAX_ERR       10U         /* 10 �δ����������� */
#define CODE_LOCK_MS       30000U      /* ����ʱ�� 30 �� */
#define CODE_BEEP_OK_MS    100U        /* У����ȷ��ʾ�� 100ms */
#define CODE_BEEP_ERR_MS   1000U       /* У������� 1 �� */

typedef enum {
    CODE_STATE_INPUT = 0,    /* ������״̬ */
    CODE_STATE_LOCKED        /* �����ޣ����� 30 �뵹��ʱ */
} code_state_e;

void CodeCheck_Init(void);                    /* ��ʼ��ģ�飬�� Flash �� current_code */
void CodeCheck_OnKey0(void);                  /* KEY0 ���£��л�����һλ */
void CodeCheck_OnKey1(void);                  /* KEY1 ���£���ǰλ���� +1 */
void CodeCheck_OnConfirm(void);               /* ȷ�ϼ���У�� 6 λ���� */
code_state_e CodeCheck_GetState(void);        /* ��ȡ״̬������ʱ�Զ��ж������Ƿ��ڣ�*/
uint8_t CodeCheck_GetCursor(void);            /* ��ȡ��ǰ����λ 0~5 */
uint8_t CodeCheck_GetErrCnt(void);            /* ��ȡ��ǰ������� */
uint32_t CodeCheck_GetRemainLockMs(void);     /* ��ȡʣ������ʱ�� ms */
void CodeCheck_GetInputBuf(char *out, uint8_t len);  /* �������뻺�壨LCD ��ʾ�ã�*/
void CodeCheck_OnDigit(uint8_t digit);        /* D10 �������������� 0-9��д�굱ǰλ�Զ���λ */
void CodeCheck_OnClear(void);                 /* D10 ������ C ����������������� */
void CodeCheck_BeepPoll(void);               /* Non-blocking beep: call from TaskLcdUI 20ms loop */
uint8_t CodeCheck_GetInputLen(void);          /* D10��������λ�� 0~6���� LCD ��ʾ */

#endif /* __CODE_CHECK_H */
