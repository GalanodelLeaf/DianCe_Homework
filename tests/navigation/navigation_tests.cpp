#include <assert.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "platform_stubs.h"
#include "../../components/lvgl_ui/tileview/satelite_info_tile.cpp"

static std::string uart_input;
static float test_heading = 0;

extern "C" int uart_read_bytes(int, void *buffer, unsigned int size, uint32_t ticks)
{
    assert(ticks == 0); // Reading GPS must not stall map dragging.
    const size_t length = std::min((size_t)size, uart_input.size());
    memcpy(buffer, uart_input.data(), length);
    uart_input.erase(0, length);
    return (int)length;
}

extern "C" bool esp_qmc5883_port_read_heading(float *heading)
{
    *heading = test_heading;
    return true;
}

static std::string nmea(const std::string &body)
{
    unsigned char checksum = 0;
    for (char ch : body) checksum ^= ch;
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
    return "$" + body + suffix;
}

static nema_rmc_data decode(const std::string &input)
{
    uart_input = input;
    nema_rmc_data result;
    neo_decode(neo_port_listen(), &result);
    return result;
}

static void test_gps_parser()
{
    const auto valid = nmea("GNRMC,120000.00,A,3954.5220,N,11623.8500,E,0.0,0.0,031126,,,A");
    auto fix = decode(valid);
    assert(fix.status == 'A');
    assert(fabs(fix.latitude - 39.9087) < 1e-8);
    assert(fabs(fix.longitude - 116.3975) < 1e-8);
    assert(decode("").status == 0);
    assert(decode(nmea("GNGGA,120000.00,,,,,0")).status == 0);

    for (size_t split = 1; split < valid.find('*') + 3; ++split) {
        assert(decode(valid.substr(0, split)).status == 0);
        assert(decode(valid.substr(split)).status == 'A');
    }
    assert(decode(nmea("GNRMC,120001.00,V,,,,,,,031126,,,N")).status == 'V');
    fix = decode(nmea("GPRMC,120000.00,A,3350.0000,S,07030.0000,W,0,0,031126,,"));
    assert(fix.status == 'A');
    assert(fabs(fix.latitude + 33.8333333333) < 1e-8 && fix.longitude == -70.5);
    auto bad_checksum = valid;
    bad_checksum[12] ^= 1;
    assert(decode(bad_checksum).status == 0);
    assert(decode(nmea("GNRMC,120000.00,A,3960.0,N,11623.85,E,0,0,031126,,")).status == 0);
    assert(decode(nmea("GNRMC,120000.00,A,nan,N,11623.85,E,0,0,031126,,")).status == 0);
    assert(decode("$" + std::string(1023, 'X')).status == 0);
    assert(decode(valid).status == 'A');
    assert(decode(valid + nmea("GNRMC,120001.00,V,,,,,,,031126,,,N")).status == 'V');
    puts("PASS: GPS decimal degrees, GN/GP, all packet split points, no stale fixes, V, checksum and overflow.");
}

static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *)
{
    lv_disp_flush_ready(driver);
}

static uint8_t *staging[9];

static void render_requested_map()
{
    MapTile requested[9];
    memcpy(requested, maps, sizeof(requested));
    show_loaded_map(requested, staging);
    lv_obj_update_layout(lv_layer_top());
}

static void assert_marker(int x, int y, bool visible)
{
    lv_obj_update_layout(lv_scr_act());
    lv_obj_update_layout(lv_layer_top());
    assert(lv_obj_has_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN) == !visible);
    if (visible) {
        lv_area_t coords;
        lv_obj_get_coords(navigation_arrow, &coords);
        assert(coords.x1 + NAVIGATION_ARROW_SIZE_PX / 2 == x);
        assert(coords.y1 + NAVIGATION_ARROW_SIZE_PX / 2 == y);
    }
}

static void move_fix_to(int x, int y)
{
    lv_area_t tile;
    lv_obj_get_coords(maps[4].map_obj, &tile);
    _lv_obj_scroll_by_raw(map_parent, x - tile.x1 - displayed_fix_offset.x,
                         y - tile.y1 - displayed_fix_offset.y);
}

static void test_map_navigation()
{
    lv_init();
    static lv_color_t pixels[320 * 10];
    static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, pixels, NULL, 320 * 10);
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = 320;
    driver.ver_res = 480;
    driver.draw_buf = &buffer;
    driver.flush_cb = flush;
    lv_disp_drv_register(&driver);
    satelite_info_tile_init(lv_scr_act());
    assert(default_map_shown && !has_real_fix);
    assert(maps[4].x == (int)lon_to_x(116.3975, cur_z));
    assert(maps[4].y == (int)lat_to_y(39.9087, cur_z));
    assert(lv_obj_has_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN));
    for (auto &pixels : staging) pixels = (uint8_t *)calloc(256 * 256, 2);
    render_requested_map();
    assert_marker(160, 240, true);

    const lv_point_t positions[] = {{95, 160}, {0, 240}, {-1, 240}, {0, 240},
        {319, 240}, {320, 240}, {319, 240}, {160, 0}, {160, -1}, {160, 0},
        {160, 479}, {160, 480}, {160, 479}, {-100, -100}, {160, 240}};
    for (const auto &point : positions) {
        move_fix_to(point.x, point.y);
        assert_marker(point.x, point.y,
            point.x >= 0 && point.x < 320 && point.y >= 0 && point.y < 480);
    }
    move_fix_to(95, 160);
    for (float heading : {0.0f, 90.0f, 180.0f, 270.0f, 359.9f}) {
        test_heading = heading;
        qmc_5883_listen_cb(NULL);
        assert_marker(95, 160, true);
        assert(lv_img_get_angle(navigation_arrow) == lroundf(heading * 10));
    }
    move_fix_to(-10, 160);
    test_heading = 135;
    qmc_5883_listen_cb(NULL);
    assert_marker(-10, 160, false);
    move_fix_to(95, 160);
    assert_marker(95, 160, true);
    assert(lv_img_get_angle(navigation_arrow) == 1350);

    const auto valid = nmea("GNRMC,120000.00,A,4000.0,N,11700.0,E,0,0,031126,,");
    uart_input = valid;
    neo_m8n_listen_cb(NULL);
    assert(has_real_fix && !default_map_shown);
    render_requested_map();
    assert_marker(95, 160, true); // GPS update preserves the user's map pan.
    lv_tick_inc(2999);
    neo_m8n_listen_cb(NULL);
    assert(has_real_fix && !default_map_shown);
    lv_tick_inc(1);
    neo_m8n_listen_cb(NULL);
    assert(!has_real_fix && default_map_shown);
    assert(maps[4].x == (int)lon_to_x(116.3975, cur_z));
    render_requested_map();
    assert_marker(95, 160, true);
    const auto revision = map_revision;
    neo_m8n_listen_cb(NULL);
    assert(map_revision == revision); // No repeated default-map loads.

    uart_input = valid;
    neo_m8n_listen_cb(NULL);
    assert(has_real_fix);
    uart_input = nmea("GNRMC,120001.00,V,,,,,,,031126,,,N");
    neo_m8n_listen_cb(NULL);
    assert(!has_real_fix && default_map_shown);
    uart_input = valid;
    neo_m8n_listen_cb(NULL);
    assert(has_real_fix && !default_map_shown);
    render_requested_map();
    assert_marker(95, 160, true);
    // Exercise the real LVGL renderer after pointer swaps and marker rotation.
    lv_refr_now(NULL);
    puts("PASS: real LVGL scrolling on all four edges, re-entry, pivot/heading, GPS loss/recovery and default location.");
}

int main()
{
    test_gps_parser();
    test_map_navigation();
    return 0;
}
