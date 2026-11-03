#include "satelite_info_tile.h"
#include "esp_lvgl_port.h"
#include "images/navigation_arrow.h"
#include <math.h>

static SemaphoreHandle_t map_update_semphr;
static uint32_t map_revision = 0;

static lv_obj_t *navigation_arrow;
static lv_obj_t *map_parent;
// 定位点在已经显示的中央瓦片内的像素偏移，只在地图刷新时更新。
static lv_point_t displayed_fix_offset;
static bool map_display_ready = false;

lv_obj_t *connection_label;
lv_obj_t *zoom_in_btn;
lv_obj_t *zoom_out_btn;
lv_obj_t *lon_label;
lv_obj_t *lat_label;
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

// 默认位置：北京天安门（接收不到有效定位信息时，以此处为中心显示地图）
#define DEFAULT_LATITUDE 39.9087
#define DEFAULT_LONGITUDE 116.3975
// 只容忍短暂缺少完整 RMC 报文；明确无效或超时后回退到天安门。
static constexpr uint32_t GPS_FIX_TIMEOUT_MS = 3000;
static bool has_real_fix = false;
static uint32_t last_fix_tick = 0;
// 默认位置的地图是否已经加载过（避免每秒重复读取SD卡）
static bool default_map_shown = false;

// 从瓦片的实际屏幕坐标计算定位点，包含拖动、惯性滚动和回弹位移。
// 顶层箭头不参与地图滚动范围计算，隐藏/恢复也不会改变地图位置。
static void navigation_arrow_update_position(void)
{
    if (navigation_arrow == nullptr || !map_display_ready) return;

    lv_area_t tile_coords;
    lv_obj_get_coords(maps[4].map_obj, &tile_coords);
    const lv_coord_t center_x = tile_coords.x1 + displayed_fix_offset.x;
    const lv_coord_t center_y = tile_coords.y1 + displayed_fix_offset.y;
    lv_disp_t *display = lv_obj_get_disp(map_parent);
    lv_area_t viewport;
    lv_obj_get_content_coords(map_parent, &viewport);
    const bool visible = center_x >= 0 && center_x < lv_disp_get_hor_res(display) &&
                         center_y >= 0 && center_y < lv_disp_get_ver_res(display) &&
                         center_x >= viewport.x1 && center_x <= viewport.x2 &&
                         center_y >= viewport.y1 && center_y <= viewport.y2;
    if (!visible) {
        lv_obj_add_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_area_t overlay_coords;
    lv_obj_get_content_coords(lv_obj_get_parent(navigation_arrow), &overlay_coords);
    lv_obj_set_pos(navigation_arrow,
                   center_x - overlay_coords.x1 - NAVIGATION_ARROW_SIZE_PX / 2,
                   center_y - overlay_coords.y1 - NAVIGATION_ARROW_SIZE_PX / 2);
    lv_obj_clear_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN);
}

static void map_scroll_cb(lv_event_t *event)
{
    navigation_arrow_update_position();
}

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
    ++map_revision;
}

// 以默认位置（北京天安门）为中心加载地图
static void map_show_default_location(void)
{
    double your_x = lon_to_x(DEFAULT_LONGITUDE, cur_z);
    double your_y = lat_to_y(DEFAULT_LATITUDE, cur_z);
    map_coord_update(your_x, your_y);
    // 发信号通知重新渲染地图
    xSemaphoreGive(map_update_semphr);
}

// 浮点数经纬度转换为度分秒格式
void lat_lon_convert(char *out, bool is_lon)
{
    int degrees, minutes;
    double frac_min, seconds;
    //
    double l = is_lon ? rmc.longitude : rmc.latitude;
    degrees  = (int)l;
    frac_min = (l - degrees) * 60.0;
    minutes  = (int)frac_min;
    seconds  = (frac_min - minutes) * 60.0;
    // 看能否显示得下再做调整
    if (is_lon)
    {
        sprintf(out, "%d°%02d'%05.3f\"%c", degrees, minutes, seconds, rmc.ew_indicator);
    }
    else
    {
        sprintf(out, "%03d°%02d'%05.3f\"%c", degrees, minutes, seconds, rmc.ns_indicator);
    }
    
}

// 接受定位信息
static void neo_m8n_listen_cb(lv_timer_t *timer)
{
    double your_x, your_y;
    int len = neo_port_listen();
    char lonlat_str[16];
    // 即使这次没有 RMC 报文，也不能复用上次的 A 状态。
    neo_decode(len, &rmc);
    if (len > 0)
    {
        // 只有状态为A才认为是有效定位（解析失败时status为0，也不能当作有效定位）
        if (rmc.status == 'A')
        {
            has_real_fix = true;
            last_fix_tick = lv_tick_get();
            default_map_shown = false;
            lv_label_set_text(connection_label, "Satellites connected :-)");
            //
            lat_lon_convert(lonlat_str, true);
            lv_label_set_text(lon_label, lonlat_str);
            lat_lon_convert(lonlat_str, false);
            lv_label_set_text(lat_label, lonlat_str);

            your_x = lon_to_x(rmc.longitude, cur_z);
            your_y = lat_to_y(rmc.latitude, cur_z);
            map_coord_update(your_x, your_y);
            // 发信号通知重新渲染地图
            xSemaphoreGive(map_update_semphr);
            return;
        }
    }

    // V 表示明确失去定位；无报文/报文不完整则最多保留最近定位 3 秒。
    if (!has_real_fix || rmc.status == 'V' ||
        lv_tick_elaps(last_fix_tick) >= GPS_FIX_TIMEOUT_MS)
    {
        has_real_fix = false;
        lv_label_set_text(connection_label, "No fix, default: Tiananmen");
        lv_label_set_text(lon_label, "lon:null");
        lv_label_set_text(lat_label, "lat:null");
        if (!default_map_shown)
        {
            default_map_shown = true;
            map_show_default_location();
        }
    }
}

// 接受磁力计信息
static void qmc_5883_listen_cb(lv_timer_t *timer)
{
    float heading_deg;
    if (navigation_arrow != nullptr && esp_qmc5883_port_read_heading(&heading_deg)) {
        // LVGL 使用 0.1 度、顺时针为正；不取反、不添加偏移。
        // 与串口输出使用同一次采样，读不到新数据时保持上一次方向。
        float angle = fmodf(heading_deg, 360.0f);
        if (angle < 0.0f) angle += 360.0f;
        const int16_t angle_tenths = (int16_t)(lroundf(angle * 10.0f) % 3600);
        // 此回调在 LVGL 任务内运行，无需另建任务修改图像。
        lv_img_set_angle(navigation_arrow, angle_tenths);
    }
}

// 缺失/不完整瓦片显示空白，不沿用其它地点的旧地图。
static void load_map_pixels(uint8_t *pixels, int zoom, int x, int y)
{
    constexpr size_t tile_bytes = 256 * 256 * 2;
    memset(pixels, 0xff, tile_bytes);
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/maps/%d/%d/%d", zoom, y, x);
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW("SD", "Missing map tile: %s", path);
        return;
    }
    const size_t bytes_read = fread(pixels, 1, tile_bytes, file);
    fclose(file);
    if (bytes_read != tile_bytes) {
        memset(pixels, 0xff, tile_bytes);
        ESP_LOGW("SD", "Incomplete map tile: %s", path);
    }
}

// 在 LVGL 锁内一次性提交同一定位的九张瓦片和箭头位置。
static void show_loaded_map(const MapTile *loaded, uint8_t **staging)
{
    for (int i = 0; i < 9; ++i) {
        // 描述符地址不变，替换像素前清除旧的图片解码缓存。
        lv_img_cache_invalidate_src(&maps[i].map_dsc);
        uint8_t *old_pixels = maps[i].map_data;
        maps[i].map_data = staging[i];
        staging[i] = old_pixels;
        maps[i].map_dsc.data = maps[i].map_data;
        lv_img_set_src(maps[i].map_obj, &maps[i].map_dsc);
        lv_obj_set_pos(maps[i].map_obj, loaded[i].px, loaded[i].py);
    }
    // map_coord_update 将定位点放在地图内容坐标 (160, 240)。
    // 使用与瓦片相同的整数像素位置，避免舍入产生偏移。
    displayed_fix_offset.x = 160 - (lv_coord_t)loaded[4].px;
    displayed_fix_offset.y = 240 - (lv_coord_t)loaded[4].py;
    map_display_ready = true;
    lv_obj_update_layout(map_parent);
    navigation_arrow_update_position();
}

// SD 卡读取使用独立缓冲；屏幕仍可滚动，且不会读到只更新了一半的地图。
static void maps_update_task(void *arg)
{
    uint8_t *staging[9];
    for (int i = 0; i < 9; ++i) {
        staging[i] = (uint8_t *)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
        configASSERT(staging[i] != nullptr);
    }
    while (1) {
        if (xSemaphoreTake(map_update_semphr, portMAX_DELAY) != pdTRUE) continue;
        MapTile requested[9];
        uint32_t revision;
        int zoom;
        if (!lvgl_port_lock(0)) continue;
        memcpy(requested, maps, sizeof(requested));
        revision = map_revision;
        zoom = cur_z;
        lvgl_port_unlock();

        for (int i = 0; i < 9; ++i) {
            load_map_pixels(staging[i], zoom, requested[i].x, requested[i].y);
        }

        if (lvgl_port_lock(0)) {
            // 读取期间如果 GPS/默认定位已切换，丢弃旧请求，下一轮读新地图。
            if (revision == map_revision) show_loaded_map(requested, staging);
            lvgl_port_unlock();
        }
    }
}

// 缩放按钮似乎按动无响应，还需调试
// static void zoom_in_btn_event_handler(lv_event_t *e)
// {
//     if (cur_z < 16) cur_z += 2;
// }

// static void zoom_out_btn_event_handler(lv_event_t *e)
// {
//     if (cur_z > 12) cur_z -= 2;
// }

void satelite_info_init()
{
    for (int i = 0; i < 9; i++)
    {
        maps[i].map_data = (uint8_t*)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
        configASSERT(maps[i].map_data != nullptr);
        maps[i].px = (i % 3) * 256;
        maps[i].py = (i / 3) * 256;
        maps[i].map_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        maps[i].map_dsc.header.w = 256;
        maps[i].map_dsc.header.h = 256;
        maps[i].map_dsc.data_size = 256 * 256 * 2;
        maps[i].map_dsc.data = maps[i].map_data;
    }
    // 创建二值信号量
    map_update_semphr = xSemaphoreCreateBinary();
    // 创建监听任务
    xTaskCreatePinnedToCore(maps_update_task, "maps_update_task", 4096, NULL, 2, NULL, 1);
    // 每100ms非阻塞读取串口，完整 RMC 到达后才更新定位。
    lv_timer_create(neo_m8n_listen_cb, 100, NULL);
    // 每200ms接收一次磁力计的信息
    lv_timer_create(qmc_5883_listen_cb, 200, NULL);
    // 开机立即使用默认定位，不等待首条 GPS 报文。
    default_map_shown = true;
    lv_label_set_text(connection_label, "No fix, default: Tiananmen");
    map_show_default_location();
}

void satelite_info_tile_init(lv_obj_t *parent)
{
    map_parent = parent;
    lv_obj_t *top_layer = lv_layer_top();
    lv_obj_t *label;

    connection_label = lv_label_create(top_layer);
    zoom_in_btn = lv_btn_create(top_layer);
    zoom_out_btn = lv_btn_create(top_layer);
    lon_label = lv_label_create(top_layer);
    lat_label = lv_label_create(top_layer);
    for (int i = 0; i < 9; i++)
    {
        maps[i].map_obj = lv_img_create(parent);
    }

    // 箭头位于地图上层，但位置由地图上的定位点决定。
    // 3.5 英寸、320x480、方形像素：8 * sqrt(320²+480²) / (3.5*25.4)
    // = 51.91 像素，图片取最接近的 52x52 像素（约 8.01x8.01 mm）。
    navigation_arrow = lv_img_create(top_layer);
    lv_img_set_src(navigation_arrow, &navigation_arrow_img);
    lv_img_set_pivot(navigation_arrow, NAVIGATION_ARROW_SIZE_PX / 2,
                     NAVIGATION_ARROW_SIZE_PX / 2);
    lv_img_set_antialias(navigation_arrow, true);
    lv_obj_clear_flag(navigation_arrow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(navigation_arrow, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    // 滚动事件在地图子对象移动之后发出，可立即同步箭头位置及可见性。
    lv_obj_add_event_cb(map_parent, map_scroll_cb, LV_EVENT_SCROLL, NULL);

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
    // lv_obj_add_event_cb(zoom_in_btn, zoom_in_btn_event_handler, LV_EVENT_CLICKED, NULL);
    // 缩小按钮
    lv_obj_set_size(zoom_out_btn, 50, 50);
    lv_obj_add_flag(zoom_out_btn, LV_OBJ_FLAG_FLOATING);
    label = lv_label_create(zoom_out_btn);
    lv_label_set_text(label, "-");
    lv_obj_center(label);
    lv_obj_align(zoom_out_btn, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    // lv_obj_add_event_cb(zoom_out_btn, zoom_out_btn_event_handler, LV_EVENT_CLICKED, NULL);

    // 两个浮动的label用于显示经纬度（转化为度分秒格式）
    lv_obj_set_size(lon_label, 100, 25);
    lv_obj_add_flag(lon_label, LV_OBJ_FLAG_FLOATING);
    lv_label_set_text(lon_label, "lon:null");

    lv_obj_set_size(lat_label, 100, 25);
    lv_obj_add_flag(lat_label, LV_OBJ_FLAG_FLOATING);
    lv_label_set_text(lat_label, "lat:null");

    // 设置一些内容的初始化
    satelite_info_init();
}
