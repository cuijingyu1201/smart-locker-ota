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
#include "iap.h"    /* IAP_ProcessSerial ???? Ymodem ???? */
#include "boot_ota.h"   /*  WiFi_IAP_Init / WiFi_IAP_Process */

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
	MX_USART2_UART_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
	
	CRC32_InitTable();   
	

  printf("\r\n\r\n========== Bootloader v1.0 ==========\r\n");
  printf("Flash: Bootloader @ 0x08000000 (32KB)\r\n");
  printf("       APP        @ 0x08008000 (464KB)\r\n");
  printf("       OTA_FLAG   @ 0x0807F000\r\n");

  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);  
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);    


{
    int load_ok = FlashParam_Load(&g_param_buf);

    if (load_ok == 0 && g_param_buf.ota_request_magic == MAGIC_OTA_REQUEST)
    {
     
        printf("\r\n[BOOT] =================================================\r\n");
        printf("[BOOT] OTA_REQUEST FLAG (magic=0x%08X) detected!\r\n",
               g_param_buf.ota_request_magic);
        printf("[BOOT]   Expected new FW: size=%u bytes, CRC=0x%08X\r\n",
               g_param_buf.ota_new_fw_size, g_param_buf.ota_new_fw_crc32);
        printf("[BOOT]   Target version: %u.%u.%u.%u\r\n",
               g_param_buf.ota_new_fw_ver_major,
               g_param_buf.ota_new_fw_ver_minor,
               g_param_buf.ota_new_fw_ver_patch,
               g_param_buf.ota_new_fw_build_num);
        printf("[BOOT] =================================================\r\n");

        FlashParam_ClearOtaRequest();
        printf("[BOOT] OTA_request flag pre-cleared (safe for fail-retries)\r\n");

        printf("[BOOT] Enter WiFi-OTA pull (D12).\r\n");
        printf("[BOOT] To force Serial IAP rescue: KEY0 + Reset\r\n");
        HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);

        if (WiFi_IAP_Init() == 0) {
            int rc = WiFi_IAP_Process(g_param_buf.ota_new_fw_crc32,
                                      g_param_buf.ota_new_fw_size,
                                      g_param_buf.ota_new_fw_ver_major,
                                      g_param_buf.ota_new_fw_ver_minor,
                                      g_param_buf.ota_new_fw_ver_patch,
                                      g_param_buf.ota_new_fw_build_num);
            if (rc != 0) {
                printf("[BOOT] WiFi_IAP_Process FAIL (rc=%d)\r\n", rc);
            }
        } else {
            printf("[BOOT] WiFi_IAP_Init FAIL\r\n");
        }

    }

    else if (FLASH_ReadWord(OTA_FLAG_ADDR) == MAGIC_OLD_OTA_FLAG) {
        printf("[BOOT] Legacy OTA_FLAG (0xA5A5A5A5) detected, clearing...\r\n");
        FLASH_ErasePage(OTA_FLAG_ADDR);
        
    }
}

  HAL_Delay(100);   

  HAL_Delay(100);   
  if (HAL_GPIO_ReadPin(KEY0_GPIO_Port, KEY0_Pin) == GPIO_PIN_RESET)
  {
      printf("\r\n[BOOT] =========== KEY0+Reset FORCE IAP MODE ===========\r\n");
      printf("[BOOT] KEY0 held on power-on. Entering Serial Ymodem IAP.\r\n");
      printf("[BOOT] Send app.bin via Ymodem (115200, 8N1) on USART1 (USB-UART).\r\n");
      printf("[BOOT] MCU will send 'C' (0x43) handshake every 5s, up to 5 retries.\r\n");
      printf("[BOOT] If Ymodem FAILs: hold KEY0 + Reset again to retry.\r\n");

      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);   
      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);   

      (void)IAP_ProcessSerial();

      printf("\r\n[BOOT] !!!!! IAP FAILED !!!!!\r\n");
      printf("[BOOT] APP area may be corrupted (if erase already started).\r\n");
      printf("[BOOT] Recovery: Hold KEY0 + Press Reset -> Try IAP again.\r\n");
      printf("[BOOT] LED fast-blink indicates IAP failure state.\r\n");

      HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
      HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
      while (1) {
          HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
          HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);
          HAL_Delay(100);
      }
  }

	/* === D13新增：升级中断检测 === */
  if (FlashParam_Load(&g_param_buf) == 0 && g_param_buf.last_ota_result == 4U)
  {
      printf("[BOOT] OTA interrupted (last_ota_result=4). APP may be incomplete.\r\n");
      printf("[BOOT] APP NOT valid!\r\n");
      printf("[BOOT] Auto-entering Serial IAP rescue (Ymodem 115200 8N1)...\r\n");
      printf("[BOOT] Send app.bin now.\r\n");
      (void)IAP_ProcessSerial();
      printf("[BOOT] IAP returned. Hold KEY0+Reset to retry.\r\n");
      while (1) {
          HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
          HAL_Delay(500);
      }
  }
  else if (IsAppValid())
  {
      printf("[BOOT] APP valid, jumping to 0x%08X...\r\n", APP_FLASH_START);

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
	else  /* APP 无效 */
	{
			printf("[BOOT] APP NOT valid!\r\n");

			/* === 新增：自动进串口 IAP 救砖 === */
			printf("[BOOT] Auto-entering Serial IAP rescue (Ymodem 115200 8N1)...\r\n");
			printf("[BOOT] Send app.bin now.\r\n");
			(void)IAP_ProcessSerial();

			/* IAP 成功会内部跳 APP；走到这里说明 IAP 失败或用户退出 */
			printf("[BOOT] IAP returned. Hold KEY0+Reset to retry.\r\n");
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

struct __FILE {
    int handle;
};

FILE __stdout;
FILE __stdin;

int fputc(int ch, FILE *f)
{
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
