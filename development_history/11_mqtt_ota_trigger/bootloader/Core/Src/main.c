/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "flash_if.h"
#include "jump_to_app.h"
#include <stdio.h>
#include "flash_param.h" 
#include "iap.h"    /* IAP_ProcessSerial 串口 Ymodem 升级 */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/*参数区缓冲区，放 BSS 段，不用栈（flash_param_t 有 4096 字节，栈放不下）*/
flash_param_t g_param_buf;
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
	
	CRC32_InitTable();    // 初始化 CRC32 表，否则 FlashParam_Load 会算出错误 CRC
	
  /* ===== printf 重定向 ===== */
  /* （放在 fputc 里更规范，这里先简单实现）*/

  /* ===== Bootloader 启动信息 ===== */
  printf("\r\n\r\n========== Bootloader v1.0 ==========\r\n");
  printf("Flash: Bootloader @ 0x08000000 (32KB)\r\n");
  printf("       APP        @ 0x08008000 (464KB)\r\n");
  printf("       OTA_FLAG   @ 0x0807F000\r\n");

  /* LED0 常亮 = 在 Bootloader 里 */
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);  /* 亮 */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);    /* 灭 */

  /* ===== 三分支状态机 ===== */

  /* ====== 分支1：检查参数区里的 OTA 请求标志 ====== */
{
    int load_ok = FlashParam_Load(&g_param_buf);

    if (load_ok == 0 && g_param_buf.ota_request_magic == MAGIC_OTA_REQUEST)
    {
        /* =============================================================
         *   分支1：参数区有有效 OTA 请求（D11 APP 写这个标志）
         *   D11 改造：进 WiFi OTA 等待死循环，D12 在这里实现拉固件
         *   救砖路径：KEY0+Reset → 标志已清 → 走分支2 串口 IAP
         * ============================================================= */

        /* 打印 OTA 请求信息（排障用：确认 D11 APP 写对了 size/crc）*/
        printf("\r\n[BOOT] =================================================\r\n");
        printf("[BOOT] OTA_REQUEST FLAG (magic=0x%08X) detected!\r\n",
               g_param_buf.ota_request_magic);
        printf("[BOOT]   Expected new FW: size=%u bytes, CRC=0x%08X\r\n",
               g_param_buf.ota_new_fw_size, g_param_buf.ota_new_fw_crc32);
        printf("[BOOT] =================================================\r\n");

        /* 先清 OTA 请求标志（防升级失败后复位又进这里死循环）*/
        FlashParam_ClearOtaRequest();
        printf("[BOOT] OTA_request flag pre-cleared (safe for fail-retries)\r\n");

        /* D11: 进 WiFi OTA 等待状态（LED0 双闪），D12 在这里实现拉固件 */
        printf("[BOOT] Enter WiFi-OTA wait (LED0 blink 200ms).\r\n");
        printf("[BOOT] D12 will pull firmware here. To force Serial IAP rescue: KEY0 + Reset\r\n");
        HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);   /* 先灭，准备双闪 */

        for (;;)
        {
            HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
            HAL_Delay(200);   /* LED0 双闪，200ms 周期，表示在 Bootloader 等升级 */

            /* TODO D12: 在这里调 WiFi_IAP_Process() 拉 APP 区固件
             *         流程：连 WiFi → 连 TCP 服务器 → HELLO/INFO/DATA/END 四步
             *         → 写 Flash + CRC32 校验 → 跳新 APP */
        }
        /* 死循环，不会走到这里，也不会再走到分支2/3 */
    }

    /* 兼容 D8 写的旧 MAGIC_OLD_OTA_FLAG，防止用户之前写了旧标志卡住 */
    else if (FLASH_ReadWord(OTA_FLAG_ADDR) == MAGIC_OLD_OTA_FLAG) {
        printf("[BOOT] Legacy OTA_FLAG (0xA5A5A5A5) detected, clearing...\r\n");
        FLASH_ErasePage(OTA_FLAG_ADDR);
        /* 清完旧标志后，下面的分支继续正常判断（不强制进 OTA 模式） */
    }
}

  /* ----- 分支 2：检查 KEY0 是否按住（串口 IAP 模式）----- */
  HAL_Delay(100);   /* 等 100ms 消抖 */
  /* ----- 分支 2：上电 KEY0 按下 → 强制进入串口 IAP 救砖模式 -----
   *   这是 OTA 系统最核心的"物理兜底通道"：
   *   任何情况下只要 KEY0+复位就能走这里，哪怕 APP 全砖、参数区全 0xFF 也能救回来。
   *   —— 工业级设计思维：只要能按到按键、能插 USB 串口，就不会变砖 */
  HAL_Delay(100);   /* 消抖 100ms：确保 KEY0 是"持续按住 100ms 以上"才判定，不是上电毛刺 */
  if (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == GPIO_PIN_RESET)
  {
      printf("\r\n[BOOT] =========== KEY0+Reset FORCE IAP MODE ===========\r\n");
      printf("[BOOT] KEY0 held on power-on. Entering Serial Ymodem IAP.\r\n");
      printf("[BOOT] Send app.bin via Ymodem (115200, 8N1) on USART1 (USB-UART).\r\n");
      printf("[BOOT] MCU will send 'C' (0x43) handshake every 5s, up to 5 retries.\r\n");
      printf("[BOOT] If Ymodem FAILs: hold KEY0 + Reset again to retry.\r\n");

      /* LED 双闪提示进入 IAP：LED0+LED1 交替闪（D8 原版是 200ms 双闪，保留同视觉提示）*/
      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);   /* LED0 亮 = 在 IAP 模式 */
      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);   /* LED1 亮 = IAP 等待传输 */

      /* 真正跑 IAP：收 Ymodem → 写 Flash → 校验 → 更新参数区 → 复位跳 APP */
      (void)IAP_ProcessSerial();

      /* 如果 IAP_ProcessSerial 走到这里 return（说明失败了）*/
      printf("\r\n[BOOT] !!!!! IAP FAILED !!!!!\r\n");
      printf("[BOOT] APP area may be corrupted (if erase already started).\r\n");
      printf("[BOOT] Recovery: Hold KEY0 + Press Reset -> Try IAP again.\r\n");
      printf("[BOOT] LED fast-blink indicates IAP failure state.\r\n");

      /* 失败死循环：LED0/LED1 以 100ms 超快闪（"急促闪"表示错误，和正常200ms IAP等待区分开）*/
      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
      while (1) {
          HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
          HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
          HAL_Delay(100);
      }
  }

  /* ----- 分支 3：APP 是否有效，有效则跳转 ----- */
  if (IsAppValid())
  {
      printf("[BOOT] APP valid, jumping to 0x%08X...\r\n", APP_FLASH_START);

      /* 可选：打印参数区信息 */
      {
          if (FlashParam_Load(&g_param_buf) == 0) {
              printf("[BOOT] Loaded param: fw=%u.%u.%u build=%u, boot_count=%u\r\n",
                     g_param_buf.fw_ver_major, g_param_buf.fw_ver_minor, g_param_buf.fw_ver_patch,
                     g_param_buf.fw_build_num, g_param_buf.boot_count);
          } else {
              printf("[BOOT] Param area uninitialized (will be set by APP)\r\n");
          }
      }

      HAL_Delay(200);
      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
      JumpToApp();
  }
  else
  {
      printf("[BOOT] APP NOT valid! Waiting for firmware...\r\n");
      printf("[BOOT] Hold KEY0 + Reset to flash via Serial IAP\r\n");
      while (1) {
          HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
          HAL_Delay(500);
      }
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* ========== printf 重定向：把 printf 输出重定向到 USART1 ========== */
struct __FILE {
    int handle;
};

FILE __stdout;
FILE __stdin;

int fputc(int ch, FILE *f)
{
    /* 把字符 ch 直接丢给 HAL_UART_Transmit 从 USART1 发出去 */
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 10);
    return ch;
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
