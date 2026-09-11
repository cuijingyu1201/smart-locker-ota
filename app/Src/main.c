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
#include "cmsis_os.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "fsmc.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "app_ipc.h"        /* 4 大 IPC 句柄：g_uart_mutex_handle / g_irq_sem_handle */
#include "app_task.h"       /* 任务函数声明 */
#include "app_uart.h"
#include "flash_param.h"
#include "ota_manager.h"
#include "app_lcd.h"
#include "app_touch.h"
#include "app_servo.h"
#include "app_sensor.h"
#include "app_ipc.h"
#include "app_buzzer.h"
#include "code_check.h"


/* ===================== fputc 重定向（D3 第二版互斥锁，原样保留） ===================== */
#ifdef __GNUC__
  #define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
  #define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif
PUTCHAR_PROTOTYPE
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 0xFFFF);
    return ch;
}

/* ===================== _write 系统调用（MicroLib printf/fputs 批量输出重定向） =====================
 * D17 关键修复：MicroLib 下带格式参数的 printf（如 lcd.c 中 printf("LCD ID:%x", id)）
 * 会调用 _write 系统调用而非 fputc。若不实现 _write，MicroLib 默认 stub 会导致程序卡死。
 * 此实现将 _write 重定向到 huart1，与 fputc/uart_printf_mutex 输出一致。
 * 注意：多任务环境下应优先使用 uart_printf_mutex（带互斥锁），避免 printf 竞争。
 * ============================================================================================ */
int _write(int fd, const char *ptr, int len)
{
    (void)fd;
    if ((ptr != NULL) && (len > 0) && (huart1.Instance != NULL)) {
        (void)HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100U);  /* 100ms 超时防 TX 故障死锁 */
    }
    return len;
}
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
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */
	__enable_irq();   /* Bootloader 跳转前 __disable_irq()，APP 必须重新开启全局中断 */
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
  MX_USART2_UART_Init();
  MX_FSMC_Init();
  MX_TIM3_Init();
  /* USER CODE BEGIN 2 */

	App_Lcd_Init();   /* LCD 初始化（FSMC 已由 MX_FSMC_Init 初始化，此处初始化 LCD 控制器 + 背光）*/
	App_Touch_Init();
	App_Servo_Init();    /* 舵机 PWM 启动，默认关锁 */
	App_Sensor_Init();   /* 霍尔/红外传感器初始化 */
	App_Buzzer_Init();   /* 蜂鸣器初始化（关闭状态） */
	
	#if APP_BUZZER_ENABLED
		App_Buzzer_Beep_Blocking(200);  /* 上电自检：响 200ms（静音模式跳过，避免无意义 200ms 延迟）*/
	#endif	
  uart_printf_mutex("\r\n############ APP v%u.%u.%u.%u (built %s %s) ############\r\n",
                    FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH, FW_BUILD_NUM,
                    __DATE__, __TIME__);
	uart_printf_mutex("\r\n===== IOT OTA APP =====\r\n");
	uart_printf_mutex("System Clock: %u Hz\r\n", SystemCoreClock);
	uart_printf_mutex("Build: %s %s\r\n", __DATE__, __TIME__);

	/* Flash maintenance belongs to boot, not to a small periodic task stack. */
	(void)FlashParam_InitOnBoot();
	
	 CodeCheck_Init();   /* D8：取件码模块初始化，从 Flash 读正确码 */

	
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

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
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    /* 只处理 WK_UP(PA0) 按键触发的 EXTI0 中断 */
    if (GPIO_Pin == KEY_WKUP_Pin)
    {
        /* =====================【CMSIS_V2 标准写法，1 行搞定】=====================
         * osSemaphoreRelease(信号量句柄)
         * 功能：在中断里 Give 1 次二值信号量 → 唤醒 TaskSemHandle。
         * 好处：
         *   ① 不用 xHigherPriorityTaskWoken 变量
         *   ② 不用 portYIELD_FROM_ISR
         *   ③ 任务/ISR 通用，内部自动判断上下文
         *   ④ CubeMX CMSIS_V2 模式下符号永远导出，不再报 Undefined symbol
         * ========================================================================= */
        if (g_irq_sem_handle != NULL) {
            osSemaphoreRelease(g_irq_sem_handle);
        }
    }
}

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

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
     ex: uart_printf_mutex("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
