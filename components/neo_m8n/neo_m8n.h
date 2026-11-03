#pragma once

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "esp_err.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "nema0183.h"

#define NEO_BUF_MAX 1024
#define NEO_LINE_MAX 256

// GPS模块使用UART1（UART0被用作日志控制台，不能再和GPS共用）
#define NEO_UART_NUM UART_NUM_1
// 引脚按开发板实际接线：GPS的TX接GPIO44，GPS的RX接GPIO43
// 因此ESP32-S3侧：RX = GPIO44，TX = GPIO43
#define NEO_UART_RX_PIN GPIO_NUM_44
#define NEO_UART_TX_PIN GPIO_NUM_43

#ifdef __cplusplus
extern "C" {
#endif

void neo_port_init(void);
int neo_port_listen(void);
// status: A=本批次有效定位（十进制度），V=明确无定位，0=暂无完整有效RMC。
// 保留跨读取批次的半条报文；len <= 0 时清除输出状态但保留半条报文。
void neo_decode(int len, struct nema_rmc_data *rmc);

// 经纬度的输入全部以角度形式

/**
 * @brief 当前经度坐标转化为球面墨卡托投影的x坐标
 */
double lon_to_x(double lon, int z);
/**
 * @brief 当前纬度坐标转化为球面墨卡托投影的y坐标
 */
double lat_to_y(double lat, int z);

#ifdef __cplusplus
}
#endif
