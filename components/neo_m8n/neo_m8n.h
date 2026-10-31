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

#ifdef __cplusplus
extern "C" {
#endif

void neo_port_init(void);
int neo_port_listen(void);
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