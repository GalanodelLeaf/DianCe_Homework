#include "satelite_info_tile.h"

SemaphoreHandle_t rend_semphr, map_update_semphr;

lv_obj_t *connection_label;
lv_obj_t *zoom_in_btn;
lv_obj_t *zoom_out_btn;
/**
 * +---+---+---+
 * | 0 | 1 | 2 |
 * +---+---+---+
 * | 3 | 4 | 5 |
 * +---+---+---+
 * | 6 | 7 | 8 |
 * +---+---+---+
 */
struct MapTile maps[9];
struct nema_rmc_data rmc;
int cur_z = 12;

static void map_coord_update(double your_x, double your_y)
{
    maps[4].px = 160 - 256 * (your_x - (int)your_x);
    maps[4].py = 240 - 256 * (your_y - (int)your_y);
    maps[4].x = (int)your_x;
    maps[4].y = (int)your_y;

    maps[0].px = maps[4].px - 256;
    maps[0].py = maps[4].py - 256;
    maps[0].x = maps[4].x - 1;
    maps[0].y = maps[4].y - 1;

    maps[1].px = maps[4].px;
    maps[1].py = maps[4].py - 256;
    maps[1].x = maps[4].x;
    maps[1].y = maps[4].y - 1;

    maps[2].px = maps[4].px + 256;
    maps[2].py = maps[4].py - 256;
    maps[2].x = maps[4].x + 1;
    maps[2].y = maps[4].y - 1;

    maps[3].px = maps[4].px - 256;
    maps[3].py = maps[4].py;
    maps[3].x = maps[4].x - 1;
    maps[3].y = maps[4].y;

    maps[5].px = maps[4].px + 256;
    maps[5].py = maps[4].py;
    maps[5].x = maps[4].x + 1;
    maps[5].y = maps[4].y;

    maps[6].px = maps[4].px - 256;
    maps[6].py = maps[4].py + 256;
    maps[6].x = maps[4].x - 1;
    maps[6].y = maps[4].y + 1;

    maps[7].px = maps[4].px;
    maps[7].py = maps[4].py + 256;
    maps[7].x = maps[4].x;
    maps[7].y = maps[4].y + 1;

    maps[8].px = maps[4].px + 256;
    maps[8].py = maps[4].py + 256;
    maps[8].x = maps[4].x + 1;
    maps[8].y = maps[4].y + 1;
}

// 接受定位信息
static void neo_m8n_listen_cb(lv_timer_t *timer)
{
    double your_x, your_y;
    int len = neo_port_listen();
    if (len > 0)
    {
        neo_decode(len, &rmc);
        if (rmc.status == 'V')
        {
            lv_label_set_text(connection_label, "Connecting satellites...");
        }
        else
        {
            lv_label_set_text(connection_label, "Satellites connected :-)");

            your_x = lon_to_x(rmc.longitude, cur_z);
            your_y = lat_to_y(rmc.latitude, cur_z);
            map_coord_update(your_x, your_y);
            // 发信号通知重新渲染
            xSemaphoreGive(map_update_semphr);
        }
    }
}

// 负责性能开销较大的地图加载和销毁管理工作
static void maps_update_task(void *arg)
{
    int if_rerend;
    while (1)
    {
        if_rerend = 0;
        if (xSemaphoreTake(map_update_semphr, portMAX_DELAY) == pdTRUE)
        {
            // 重新分配内存池
            // 采用释放创建内存加指针分配，还是拷贝数据保证内存连续性？哪个好点呢
            // if (maps[4].px < 0)
            // {
            //     heap_caps_free(maps[0].map_data);
            //     heap_caps_free(maps[3].map_data);
            //     heap_caps_free(maps[6].map_data);
            //     maps[0].map_data = maps[1].map_data;
            //     maps[3].map_data = maps[4].map_data;
            //     maps[6].map_data = maps[7].map_data;
            //     maps[1].map_data = maps[2].map_data;
            //     maps[4].map_data = maps[5].map_data;
            //     maps[7].map_data = maps[8].map_data;
            //     maps[2].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[5].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[8].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     load_rgb565_bin_file(2, maps[4].x + 1, maps[4].y + 1);
            //     load_rgb565_bin_file(5, maps[4].x + 1, maps[4].y);
            //     load_rgb565_bin_file(8, maps[4].x + 1, maps[4].y - 1);
            //     // memcpy(maps[0].map_data, maps[1].map_data, 256*256*2);
            //     // memcpy(maps[3].map_data, maps[4].map_data, 256*256*2);
            //     // memcpy(maps[6].map_data, maps[7].map_data, 256*256*2);
            //     // memcpy(maps[1].map_data, maps[2].map_data, 256*256*2);
            //     // memcpy(maps[4].map_data, maps[5].map_data, 256*256*2);
            //     // memcpy(maps[7].map_data, maps[8].map_data, 256*256*2);
            //     if_rerend = 1;
            // }
            // if (maps[4].px > 320 - 256)
            // {
            //     heap_caps_free(maps[2].map_data);
            //     heap_caps_free(maps[5].map_data);
            //     heap_caps_free(maps[8].map_data);
            //     maps[2].map_data = maps[1].map_data;
            //     maps[5].map_data = maps[4].map_data;
            //     maps[8].map_data = maps[7].map_data;
            //     maps[1].map_data = maps[0].map_data;
            //     maps[4].map_data = maps[3].map_data;
            //     maps[7].map_data = maps[6].map_data;
            //     maps[0].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[3].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[6].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     load_rgb565_bin_file(0, maps[4].x - 1, maps[4].y + 1);
            //     load_rgb565_bin_file(3, maps[4].x - 1, maps[4].y);
            //     load_rgb565_bin_file(6, maps[4].x - 1, maps[4].y - 1);
            //     // memcpy(maps[2].map_data, maps[1].map_data, 256*256*2);
            //     // memcpy(maps[5].map_data, maps[4].map_data, 256*256*2);
            //     // memcpy(maps[8].map_data, maps[7].map_data, 256*256*2);
            //     // memcpy(maps[1].map_data, maps[0].map_data, 256*256*2);
            //     // memcpy(maps[4].map_data, maps[3].map_data, 256*256*2);
            //     // memcpy(maps[7].map_data, maps[6].map_data, 256*256*2);
            //     if_rerend = 1;
            // }
            // if (maps[4].py < 0)
            // {
            //     heap_caps_free(maps[0].map_data);
            //     heap_caps_free(maps[1].map_data);
            //     heap_caps_free(maps[2].map_data);
            //     maps[0].map_data = maps[3].map_data;
            //     maps[1].map_data = maps[4].map_data;
            //     maps[2].map_data = maps[5].map_data;
            //     maps[3].map_data = maps[6].map_data;
            //     maps[4].map_data = maps[7].map_data;
            //     maps[5].map_data = maps[8].map_data;
            //     maps[6].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[7].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[8].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     load_rgb565_bin_file(6, maps[4].x - 1, maps[4].y - 1);
            //     load_rgb565_bin_file(7, maps[4].x, maps[4].y - 1);
            //     load_rgb565_bin_file(8, maps[4].x + 1, maps[4].y - 1);
            //     // memcpy(maps[0].map_data, maps[3].map_data, 256*256*2);
            //     // memcpy(maps[1].map_data, maps[4].map_data, 256*256*2);
            //     // memcpy(maps[2].map_data, maps[5].map_data, 256*256*2);
            //     // memcpy(maps[3].map_data, maps[6].map_data, 256*256*2);
            //     // memcpy(maps[4].map_data, maps[7].map_data, 256*256*2);
            //     // memcpy(maps[5].map_data, maps[8].map_data, 256*256*2);
            //     if_rerend = 1;
            // }
            // if (maps[4].py > 480 - 256)
            // {
            //     heap_caps_free(maps[6].map_data);
            //     heap_caps_free(maps[7].map_data);
            //     heap_caps_free(maps[8].map_data);
            //     maps[6].map_data = maps[3].map_data;
            //     maps[7].map_data = maps[4].map_data;
            //     maps[8].map_data = maps[5].map_data;
            //     maps[3].map_data = maps[0].map_data;
            //     maps[4].map_data = maps[1].map_data;
            //     maps[5].map_data = maps[2].map_data;
            //     maps[0].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[1].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     maps[2].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
            //     load_rgb565_bin_file(0, maps[4].x - 1, maps[4].y + 1);
            //     load_rgb565_bin_file(1, maps[4].x, maps[4].y + 1);
            //     load_rgb565_bin_file(2, maps[4].x + 1, maps[4].y + 1);
            //     // memcpy(maps[6].map_data, maps[3].map_data, 256*256*2);
            //     // memcpy(maps[7].map_data, maps[4].map_data, 256*256*2);
            //     // memcpy(maps[8].map_data, maps[5].map_data, 256*256*2);
            //     // memcpy(maps[3].map_data, maps[0].map_data, 256*256*2);
            //     // memcpy(maps[4].map_data, maps[1].map_data, 256*256*2);
            //     // memcpy(maps[5].map_data, maps[2].map_data, 256*256*2);
            //     if_rerend = 1;
            // }

            // 直接更新所有的image，最方便，但是开销如何？
            for (int i = 0; i < 9; i++)
            {
                load_rgb565_bin_file(i, maps[i].x, maps[i].y);
            }
            if_rerend = 1;
            if (if_rerend)
            {
                xSemaphoreGive(rend_semphr);
            }
        }
        vTaskDelay(100);
    }
}

static void rerend_all_task(void *arg)
{
    while (1)
    {
        if (xSemaphoreTake(rend_semphr, portMAX_DELAY) == pdTRUE)
        {
            for (int i = 0; i < 9; i++)
            {
                lv_img_set_src(maps[i].map_obj, &maps[i].map_dsc);
                lv_obj_set_pos(maps[i].map_obj, maps[i].px, maps[i].py);
            }
        }
        vTaskDelay(100);
    }
}

static void zoom_in_btn_event_handler(lv_event_t *e)
{
    if (cur_z < 16) cur_z += 2;
}

static void zoom_out_btn_event_handler(lv_event_t *e)
{
    if (cur_z > 12) cur_z -= 2;
}

void satelite_info_init()
{
    for (int i = 0; i < 9; i++)
    {
        maps[i].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
        maps[i].px = (i % 3) * 256;
        maps[i].py = (i / 3) * 256;
        maps[i].map_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        maps[i].map_dsc.header.w = 256;
        maps[i].map_dsc.header.h = 256;
        maps[i].map_dsc.data_size = 256 * 256 * 2;
        maps[i].map_dsc.data = maps[i].map_data;
    }
    rend_semphr = xSemaphoreCreateBinary();
    map_update_semphr = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(maps_update_task, "maps_update_task", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(rerend_all_task, "rerend_all_task", 4096, NULL, 1, NULL, 0);
    lv_timer_create(neo_m8n_listen_cb, 1000, NULL);
}

void satelite_info_tile_init(lv_obj_t *parent)
{
    lv_obj_t *top_layer = lv_layer_top();
    lv_obj_t *label = lv_obj_create(parent);

    connection_label = lv_label_create(parent);
    zoom_in_btn = lv_btn_create(top_layer);
    zoom_out_btn = lv_btn_create(top_layer);
    for (int i = 0; i < 9; i++)
    {
        maps[i].map_obj = lv_img_create(parent);
    }

    lv_obj_set_style_text_font(connection_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(connection_label, LV_ALIGN_TOP_LEFT, 2, 2);

    // 设置成浮动按钮位于页面的右下角
    // 放大按钮
    lv_obj_set_size(zoom_in_btn, 50, 50);
    lv_obj_add_flag(zoom_in_btn, LV_OBJ_FLAG_FLOATING);
    label = lv_label_create(zoom_in_btn);
    lv_label_set_text(label, "+");
    lv_obj_center(label);
    lv_obj_align(zoom_in_btn, LV_ALIGN_BOTTOM_RIGHT, -5, -60);
    lv_obj_add_event_cb(zoom_in_btn, zoom_in_btn_event_handler, LV_EVENT_CLICKED, NULL);
    // 缩小按钮
    lv_obj_set_size(zoom_out_btn, 50, 50);
    lv_obj_add_flag(zoom_out_btn, LV_OBJ_FLAG_FLOATING);
    label = lv_label_create(zoom_out_btn);
    lv_label_set_text(label, "-");
    lv_obj_center(label);
    lv_obj_align(zoom_out_btn, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    lv_obj_add_event_cb(zoom_out_btn, zoom_out_btn_event_handler, LV_EVENT_CLICKED, NULL);

    // 设置一些内容的初始化
    satelite_info_init();
}

int load_rgb565_bin_file(int n, int x, int y)
{
    int i = 0;
    char path[128];
    sprintf(path, "/sdcard/maps/%d/%d/%d", cur_z, y, x);
    FILE *map = fopen(path, "rb");
    if (map == NULL)
    {
        ESP_LOGW("SD", "Failed to open bin file");
        return 0;
    }
    while(fread(&maps[n].map_data[i], sizeof(uint8_t), 1, map) == 1)
    {
        ++ i;
    }
    ESP_LOGI("SD", "Open bin file");
    return 1;
}