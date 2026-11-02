#ifndef SATELITE_INFO_TILE_H
#define SATELITE_INFO_TILE_H

#include "../lvgl_ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_log.h"

#include "esp_qmc5883_port.h"
#include "neo_m8n.h"

#define MAP_RENDER_TOP 30
#define MAP_RENDER_DOWN 480

struct MapTile
{
    int x, y;               // 墨卡托投影对应的坐标，取整数
    double px, py;          // 相对屏幕左上角的坐标
    lv_obj_t     *map_obj;
    uint8_t      *map_data;
    lv_img_dsc_t  map_dsc;
};

#ifdef __cplusplus
extern "C" {
#endif

void satelite_info_tile_init(lv_obj_t *parent);

int load_rgb565_bin_file(int n, int x, int y);

#ifdef __cplusplus
}
#endif

#endif