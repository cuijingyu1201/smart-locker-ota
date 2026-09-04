/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   This file provides code for the configuration
  *          of the USART instances.
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
#include "usart.h"

/* USER CODE BEGIN 0 */
#include <string.h>  
#include "app_ipc.h" 
/* ========= USART2(ESP8266) 接收缓冲区 ========= */
#define ESP_RX_BUF_SIZE  512
static uint8_t  esp_rx_buf[ESP_RX_BUF_SIZE];   /* 接收缓冲区 */
static uint16_t esp_rx_wr_idx = 0;             /* 写指针（ISR 中写入） */
static uint8_t  esp_rx_last_byte;              /* ISR 每次只接收 1 字节的临时变量 */

/*
 * ESP8266_GetLine：从接收缓冲区取出一行可用数据
 * 参数 line：输出缓冲区，由 char 数组提供
 * 参数 max_len：输出缓冲区最大长度
 * 返回：实际复制的字节数，0 = 没有数据/没有换行
 */
uint16_t ESP8266_GetLine(char *line, uint16_t max_len)
{
    uint16_t len;
    taskENTER_CRITICAL();                              /* 关中断，防止 ISR 竞态 */
    {
        if (esp_rx_wr_idx == 0) {                      /* 空，什么都没收到 */
            taskEXIT_CRITICAL();
            return 0;
        }
        if (esp_rx_wr_idx >= max_len) esp_rx_wr_idx = max_len - 1;  /* 超长时截断 */
        memcpy(line, esp_rx_buf, esp_rx_wr_idx);
        line[esp_rx_wr_idx] = '\0';
        len = esp_rx_wr_idx;
        esp_rx_wr_idx = 0;                             /* 清空缓冲区，准备下一行 */
    }
    taskEXIT_CRITICAL();                               /* 开中断 */
    return len;
}

/*
 * ESP8266_ClearRxBuf：手动清空接收缓冲（AT 指令切换状态时用）
 */
void ESP8266_ClearRxBuf(void)
{
    taskENTER_CRITICAL();
    esp_rx_wr_idx = 0;
    memset(esp_rx_buf, 0, ESP_RX_BUF_SIZE);
    taskEXIT_CRITICAL();
}

/*
 * ESP8266_StartReceiveIT：启动 USART2 逐字节中断接收
 * 要求 FreeRTOS IPC 初始化完成后调用，否则信号量句柄可能为 NULL
 */
void ESP8266_StartReceiveIT(void)
{
    ESP8266_ClearRxBuf();
    HAL_UART_Receive_IT(&huart2, &esp_rx_last_byte, 1);
}
/* USER CODE END 0 */

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* USART1 init function */

void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}
/* USART2 init function */

void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspInit 0 */

  /* USER CODE END USART1_MspInit 0 */
    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN USART1_MspInit 1 */

  /* USER CODE END USART1_MspInit 1 */
  }
  else if(uartHandle->Instance==USART2)
  {
  /* USER CODE BEGIN USART2_MspInit 0 */

  /* USER CODE END USART2_MspInit 0 */
    /* USART2 clock enable */
    __HAL_RCC_USART2_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART2 GPIO Configuration
    PA2     ------> USART2_TX
    PA3     ------> USART2_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USART2 interrupt Init */
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
  /* USER CODE BEGIN USART2_MspInit 1 */

  /* USER CODE END USART2_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspDeInit 0 */

  /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9|GPIO_PIN_10);

  /* USER CODE BEGIN USART1_MspDeInit 1 */

  /* USER CODE END USART1_MspDeInit 1 */
  }
  else if(uartHandle->Instance==USART2)
  {
  /* USER CODE BEGIN USART2_MspDeInit 0 */

  /* USER CODE END USART2_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART2_CLK_DISABLE();

    /**USART2 GPIO Configuration
    PA2     ------> USART2_TX
    PA3     ------> USART2_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_2|GPIO_PIN_3);

    /* USART2 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART2_IRQn);
  /* USER CODE BEGIN USART2_MspDeInit 1 */

  /* USER CODE END USART2_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
/*
 * HAL_UART_RxCpltCallback：HAL 的 UART 接收完成回调，每接收一个字节
 * 收到 1 字节就调用一次，拼接成完整报文，遇到 \n 发送 Give 信号通知任务
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)   /* 只处理 ESP8266 的 USART2，USART1/3 不处理 */
    {
        uint8_t ch = esp_rx_last_byte;   /* 接收到的 1 字节 */

        /* 1. 拼接接收报文（包括换行） */
        if (esp_rx_wr_idx < ESP_RX_BUF_SIZE - 1) {
            esp_rx_buf[esp_rx_wr_idx++] = ch;
        }

        /* 2. 检测换行 \n 或 AT 回复中的 ERROR\r\n，发送 Give 信号通知任务 */
        if (ch == '\n')
        {
            if (g_esp_rx_sem_handle != NULL) {
                osSemaphoreRelease(g_esp_rx_sem_handle);  /* CMSIS-V2 标准接口，ISR 中也可以调用 */
            }
        }

        /* 3. 继续下一字节中断接收，HAL 中断接收是"接收未完成"模式
         *    每 1 字节就停一次，需要手动启动下一次才能继续接收 */
        if (HAL_UART_Receive_IT(&huart2, &esp_rx_last_byte, 1) != HAL_OK) {
            /* 接收链断裂时 abort 后重试，防止永久失联 */
            HAL_UART_AbortReceive_IT(&huart2);
            HAL_UART_Receive_IT(&huart2, &esp_rx_last_byte, 1);
        }
    }
}

/*
 * HAL_UART_ErrorCallback：UART 发生 ORE/FE/NE 等错误时的回调
 * 不实现这个函数，HAL 会把接收状态置为错误且不再调 RxCpltCallback
 * → ESP8266 永久失联，必须重启才能恢复
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        /* 清错误标志 + abort + 重启接收 */
        HAL_UART_AbortReceive_IT(huart);
        HAL_UART_Receive_IT(huart, &esp_rx_last_byte, 1);
    }
}
/* USER CODE END 1 */

