#pragma once

#include "driver/i2c_master.h"
#include "magni.h"

extern SensorQMC5883P qmc5883;

void esp_qmc5883_port_init(i2c_master_bus_handle_t bus_handle);
void esp_qmc5883_port_output(void);
