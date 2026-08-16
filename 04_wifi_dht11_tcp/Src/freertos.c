/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_ipc.h"
#include "app_task.h"
#include <stdio.h>
#include "app_dht11.h"
#include "app_esp8266.h"
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
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* Hook prototypes */
void vApplicationIdleHook(void);
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName);
void vApplicationMallocFailedHook(void);

/* USER CODE BEGIN 2 */
#if 0
void vApplicationIdleHook( void )
{
   /* vApplicationIdleHook() will only be called if configUSE_IDLE_HOOK is set
   to 1 in FreeRTOSConfig.h. It will be called on each iteration of the idle
   task. It is essential that code added to this hook function never attempts
   to block in any way (for example, call xQueueReceive() with a block time
   specified, or call vTaskDelay()). If the application makes use of the
   vTaskDelete() API function (as this demo application does) then it is also
   important that vApplicationIdleHook() is permitted to return to its calling
   function, because it is the responsibility of the idle task to clean up
   memory allocated by the kernel to any task that has since been deleted. */
}
#endif
/* USER CODE END 2 */

/* USER CODE BEGIN 4 */
#if 0
void vApplicationStackOverflowHook(xTaskHandle xTask, signed char *pcTaskName)
{
   /* Run time stack overflow checking is performed if
   configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2. This hook function is
   called if a stack overflow is detected. */
}
#endif 
/* USER CODE END 4 */

/* USER CODE BEGIN 5 */
#if 0
void vApplicationMallocFailedHook(void)
{
   /* vApplicationMallocFailedHook() will only be called if
   configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h. It is a hook
   function that will get called if a call to pvPortMalloc() fails.
   pvPortMalloc() is called internally by the kernel whenever a task, queue,
   timer or semaphore is created. It is also called by various parts of the
   demo application. If heap_1.c or heap_2.c are used, then the size of the
   heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
   FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
   to query the size of free heap space that remains (although it does not
   provide information on how the remaining heap might be fragmented). */
}
#endif
/* USER CODE END 5 */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* 初始化 4 大 IPC（必须在创建任务之前！） */
			App_IPC_Init();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
//  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* -------------------- TaskLED -------------------- */
    const osThreadAttr_t taskLED_attr = {
        .name       = "TaskLED",
        .attr_bits  = osThreadDetached,    
        .cb_mem     = NULL,
        .cb_size    = 0,
        .stack_mem  = NULL,
        .stack_size = TASK_LED_STACK_SIZE,  /* 256 words = 1024 Bytes */
        .priority   = TASK_LED_PRIORITY,    /* osPriorityNormal = 24 */
        .tz_module  = 0,
        .reserved   = 0
    };
    osThreadId_t taskLED_handle = osThreadNew(TaskLED, NULL, &taskLED_attr);
    if (taskLED_handle == NULL) {
        printf("[ERROR] osThreadNew(TaskLED) failed!\r\n");
    }

    /* -------------------- TaskPrint -------------------- */
    const osThreadAttr_t taskPrint_attr = {
        .name       = "TaskPrint",
        .attr_bits  = osThreadDetached,
        .cb_mem     = NULL,
        .cb_size    = 0,
        .stack_mem  = NULL,
        .stack_size = TASK_PRINT_STACK_SIZE, /* 512 words = 2048 Bytes */
        .priority   = TASK_PRINT_PRIORITY,   /* osPriorityBelowNormal = 16 */
        .tz_module  = 0,
        .reserved   = 0
    };
    osThreadId_t taskPrint_handle = osThreadNew(TaskPrint, NULL, &taskPrint_attr);
    if (taskPrint_handle == NULL) {
        printf("[ERROR] osThreadNew(TaskPrint) failed!\r\n");
    }

		    /* ===== TaskKeyPoll ===== */
    const osThreadAttr_t taskKeyPoll_attr = {
        .name       = "TaskKeyPoll",
        .attr_bits  = osThreadDetached,
        .stack_size = TASK_KEY_POLL_STACK_SIZE,
        .priority   = TASK_KEY_POLL_PRIORITY,
    };
    osThreadId_t taskKeyPoll_handle = osThreadNew(TaskKeyPoll, NULL, &taskKeyPoll_attr);
    if (taskKeyPoll_handle == NULL) { printf("[ERROR] osThreadNew(TaskKeyPoll) failed!\r\n"); }

    /* ===== TaskSemHandle ===== */
    const osThreadAttr_t taskSemHandle_attr = {
        .name       = "TaskSemHandle",
        .attr_bits  = osThreadDetached,
        .stack_size = TASK_SEM_HANDLE_STACK_SIZE,
        .priority   = TASK_SEM_HANDLE_PRIORITY,
    };
    osThreadId_t taskSemHandle_handle = osThreadNew(TaskSemHandle, NULL, &taskSemHandle_attr);
    if (taskSemHandle_handle == NULL) { printf("[ERROR] osThreadNew(TaskSemHandle) failed!\r\n"); }
		    /* -------------------- TaskDHT11 -------------------- */
    const osThreadAttr_t taskDHT11_attr = {
        .name       = "TaskDHT11",
        .attr_bits  = osThreadDetached,
        .stack_size = 384 * 4,       /* 384 words = 1536B（DWT 延时 + 打印） */
        .priority   = osPriorityNormal, /* 24，和 TaskLED 同优先级 */
    };
    if (osThreadNew(TaskDHT11, NULL, &taskDHT11_attr) == NULL) {
        printf("[ERROR] osThreadNew(TaskDHT11) failed!\r\n");
    }

    /* -------------------- TaskESP8266 -------------------- */
    const osThreadAttr_t taskESP8266_attr = {
        .name       = "TaskESP8266",
        .attr_bits  = osThreadDetached,
        .stack_size = 512 * 4,       /* 512 words = 2048B（snprintf 拼接 AT+JSON 吃栈深） */
        .priority   = osPriorityNormal, /* 24，不抢实时任务 */
    };
    if (osThreadNew(TaskESP8266, NULL, &taskESP8266_attr) == NULL) {
        printf("[ERROR] osThreadNew(TaskESP8266) failed!\r\n");
    }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/* USER CODE END Application */

