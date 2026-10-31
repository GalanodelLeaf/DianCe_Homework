#pragma once

#include <math.h>

#ifndef PI
#define PI 3.14159265358979323846
#endif

// QMC5883P 与 QMC6310 寄存器完全兼容（仅 I2C 地址不同），
// 这里直接复用 SensorLib 自带的 SensorQMC6310 驱动。
#include "SensorQMC6310.hpp"

#define QMC5883P_SLAVE_ADDRESS (0x2C)

typedef SensorQMC6310 SensorQMC5883P;
