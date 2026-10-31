#include <stdio.h>

#include "esp_qmc5883_port.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

SensorQMC5883P qmc5883;

void esp_qmc5883_port_init(i2c_master_bus_handle_t bus_handle)
{
    while (!qmc5883.begin(bus_handle, QMC5883P_SLAVE_ADDRESS)) {
        printf("Failed to find QMC5883P - check your wiring!\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("QMC5883P Device ID: 0x%02x\r\n", qmc5883.getChipID());

    qmc5883.configMagnetometer(
        SensorQMC5883P::MODE_CONTINUOUS,
        SensorQMC5883P::RANGE_2G,
        SensorQMC5883P::DATARATE_100HZ,
        SensorQMC5883P::OSR_8,
        SensorQMC5883P::DSR_1);

    qmc5883.setDeclination(0.0f);
}

void esp_qmc5883_port_output(void)
{
    Polar p;
    if (qmc5883.readPolar(p)) {
        printf("QMC5883P: heading=%.1f deg | X=%d Y=%d Z=%d\r\n",
               p.polar, qmc5883.getRawX(), qmc5883.getRawY(), qmc5883.getRawZ());
    }
}
