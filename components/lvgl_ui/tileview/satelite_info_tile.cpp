#include "satelite_info_tile.h"
#include "esp_lvgl_port.h"
#include "images/navigation_arrow.h"
#include <math.h>
#include <sys/stat.h>
#include "map_tile_diagnostics.h"

static SemaphoreHandle_t map_update_semphr;
static uint32_t map_revision = 0;
static lv_obj_t *navigation_arrow;
static lv_obj_t *map_parent;
static lv_obj_t *map_status;
static lv_obj_t *map_level_label;
static lv_obj_t *recenter_btn;
lv_obj_t *connection_label;
lv_obj_t *zoom_in_btn;
lv_obj_t *zoom_out_btn;
struct MapTile maps[9];
struct nema_rmc_data rmc;
int cur_z = 12;

#define DEFAULT_LATITUDE 39.9087
#define DEFAULT_LONGITUDE 116.3975
static constexpr uint32_t GPS_FIX_TIMEOUT_MS = 3000;
static constexpr double MERCATOR_MAX_LAT = 85.0511287798066;
static bool has_real_fix = false, ever_real_fix = false, manual_view = false;
static uint32_t last_fix_tick = 0;
// Normalized Web Mercator world coordinates: independent of zoom and GPS updates.
static double view_x, view_y, fix_x, fix_y;
static bool map_display_ready = false, levels_ready = false, request_dirty = false;
static uint32_t available_levels = 0;
static int displayed_z = -1, displayed_center_x = 0, displayed_center_y = 0;
struct MapRequest { int x, y, zoom; uint32_t revision; };
static MapRequest pending_request = {-1, -1, -1, 0};

static double wrap_world(double x) { return x - floor(x); }
static double clamp_world_y(double y) { return fmax(0.0, fmin(1.0 - 1e-12, y)); }
static double world_delta(double x) { return x - floor(x + 0.5); }
static double longitude_world(double lon) { return wrap_world((lon + 180.0) / 360.0); }
static double latitude_world(double lat)
{
    lat = fmax(-MERCATOR_MAX_LAT, fmin(MERCATOR_MAX_LAT, lat));
    return clamp_world_y(lat_to_y(lat, 0));
}

// Pure selection also used by host tests. Sparse sets (e.g. 12/14/16) are legal.
static int next_map_level(uint32_t levels, int current, int direction)
{
    for (int z = current + direction; z >= 0 && z <= 19; z += direction)
        if (levels & (1u << z)) return z;
    return -1;
}

static void update_zoom_buttons()
{
    if(map_level_label) lv_label_set_text_fmt(map_level_label,"Z %d",cur_z);
    lv_obj_add_state(zoom_in_btn, LV_STATE_DISABLED);
    lv_obj_add_state(zoom_out_btn, LV_STATE_DISABLED);
    if (!levels_ready) return;
    if (next_map_level(available_levels, cur_z, 1) >= 0)
        lv_obj_clear_state(zoom_in_btn, LV_STATE_DISABLED);
    if (next_map_level(available_levels, cur_z, -1) >= 0)
        lv_obj_clear_state(zoom_out_btn, LV_STATE_DISABLED);
}

static void navigation_arrow_update_position()
{
    if (!navigation_arrow) return;
    lv_area_t viewport, overlay;
    lv_obj_get_content_coords(map_parent, &viewport);
    lv_obj_get_content_coords(lv_obj_get_parent(navigation_arrow), &overlay);
    const double scale = 256.0 * (1u << cur_z);
    const int x = viewport.x1 + lv_area_get_width(&viewport) / 2 +
                  (int)lround(world_delta(fix_x - view_x) * scale);
    const int y = viewport.y1 + lv_area_get_height(&viewport) / 2 +
                  (int)lround((fix_y - view_y) * scale);
    lv_disp_t *disp = lv_obj_get_disp(map_parent);
    if (x < viewport.x1 || x > viewport.x2 || y < viewport.y1 || y > viewport.y2 ||
        x < 0 || y < 0 || x >= lv_disp_get_hor_res(disp) || y >= lv_disp_get_ver_res(disp)) {
        lv_obj_add_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_set_pos(navigation_arrow, x - overlay.x1 - NAVIGATION_ARROW_SIZE_PX / 2,
                   y - overlay.y1 - NAVIGATION_ARROW_SIZE_PX / 2);
    lv_obj_clear_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN);
}

static void position_displayed_map()
{
    if (map_display_ready && displayed_z == cur_z) {
        const double n = (1u << cur_z);
        const int w = lv_obj_get_content_width(map_parent);
        const int h = lv_obj_get_content_height(map_parent);
        for (int i = 0; i < 9; ++i) {
            // Wrap the grid anchor, not individual tiles: z0/z1 must retain duplicate
            // world copies in distinct slots rather than collapsing onto each other.
            const double anchor = world_delta((displayed_center_x + 0.5) / n - view_x) * n * 256;
            maps[i].px = w / 2 + anchor - 128 + (i % 3 - 1) * 256;
            maps[i].py = h / 2 + (displayed_center_y / n - view_y) * n * 256 + (i / 3 - 1) * 256;
            if (maps[i].px + 256 <= 0 || maps[i].px >= w ||
                maps[i].py + 256 <= 0 || maps[i].py >= h) {
                // Do not cast distant world pixels to LVGL's small coordinate type.
                lv_obj_add_flag(maps[i].map_obj, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_pos(maps[i].map_obj, (lv_coord_t)lround(maps[i].px),
                               (lv_coord_t)lround(maps[i].py));
                lv_obj_clear_flag(maps[i].map_obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    navigation_arrow_update_position();
}

static void signal_map_request()
{
    if (levels_ready && available_levels && request_dirty) {
        request_dirty = false;
        xSemaphoreGive(map_update_semphr);
    }
}

static void request_view_map()
{
    const int n = (1u << cur_z);
    const int x = (int)floor(view_x * n), y = (int)floor(view_y * n);
    if (pending_request.x != x || pending_request.y != y || pending_request.zoom != cur_z) {
        pending_request = {x, y, cur_z, ++map_revision};
        request_dirty = true;
        if (levels_ready && available_levels) lv_label_set_text(map_status, "Loading map");
    }
    position_displayed_map();
}

static void map_request_timer(lv_timer_t *) { signal_map_request(); }

static void map_pointer_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSING) {
        lv_indev_t *input = lv_indev_get_act();
        if (!input || lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) return;
        lv_point_t delta;
        lv_indev_get_vect(input, &delta);
        if (!delta.x && !delta.y) return;
        manual_view = true;
        const double scale = 256.0 * (1u << cur_z);
        view_x = wrap_world(view_x - delta.x / scale);
        view_y = clamp_world_y(view_y - delta.y / scale);
        request_view_map();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        signal_map_request();
    }
}

static void zoom_btn_event_handler(lv_event_t *event)
{
    if (!levels_ready) return;
    const int direction = lv_event_get_target(event) == zoom_in_btn ? 1 : -1;
    const int z = next_map_level(available_levels, cur_z, direction);
    if (z < 0) return;
    manual_view = true;
    cur_z = z; // view_x/y remain the geographic anchor at the viewport center.
    for (int i = 0; i < 9; ++i) lv_obj_add_flag(maps[i].map_obj, LV_OBJ_FLAG_HIDDEN);
    map_display_ready = false;
    update_zoom_buttons();
    request_view_map();
    signal_map_request();
}

static void recenter_btn_event_handler(lv_event_t *)
{
    // A one-shot recenter, not follow mode. Keep zoom and the latest/default fix.
    manual_view = true;
    view_x = wrap_world(fix_x);
    view_y = clamp_world_y(fix_y);
    request_view_map();
    signal_map_request();
}

static void apply_available_levels(uint32_t levels)
{
    available_levels = levels & ((1u << 20) - 1);
    levels_ready = true;
    if (!available_levels) {
        lv_label_set_text(map_status, "No map zoom level");
    } else {
        if (!(available_levels & (1u << cur_z))) {
            int best = -1;
            for (int z = 0; z <= 19; ++z)
                if ((available_levels & (1u << z)) && (best < 0 || abs(z - cur_z) < abs(best - cur_z))) best = z;
            cur_z = best;
        }
        request_view_map();
        request_dirty = true;
        signal_map_request();
        if (next_map_level(available_levels, cur_z, 1) < 0 &&
            next_map_level(available_levels, cur_z, -1) < 0)
            lv_label_set_text(map_status, "No map zoom level");
    }
    update_zoom_buttons();
}

// Parser unchanged. First valid fix centers once unless the user already panned/zoomed.
// Later fixes only move the marker; loss retains the last known marker and viewport.
static void neo_m8n_listen_cb(lv_timer_t *)
{
    const int len = neo_port_listen();
    neo_decode(len, &rmc);
    if (len > 0 && rmc.status == 'A') {
        has_real_fix = true;
        last_fix_tick = lv_tick_get();
        fix_x = longitude_world(rmc.longitude);
        fix_y = latitude_world(rmc.latitude);
        if (!ever_real_fix && !manual_view) {
            view_x = fix_x; view_y = fix_y;
            request_view_map(); signal_map_request();
        }
        ever_real_fix = true;
        lv_label_set_text(connection_label, "Satellites connected :-)");
        navigation_arrow_update_position();
        return;
    }
    if (!has_real_fix || rmc.status == 'V' || lv_tick_elaps(last_fix_tick) >= GPS_FIX_TIMEOUT_MS) {
        has_real_fix = false;
        lv_label_set_text(connection_label, ever_real_fix ? "No fix, last position" : "No fix, default: Tiananmen");
    }
}

// Existing heading consumption is unchanged; no calibration or sensor collection added.
static void qmc_5883_listen_cb(lv_timer_t *)
{
    float heading_deg;
    if (navigation_arrow != nullptr && esp_qmc5883_port_read_heading(&heading_deg)) {
        float angle = fmodf(heading_deg, 360.0f);
        if (angle < 0.0f) angle += 360.0f;
        const int16_t angle_tenths = (int16_t)(lroundf(angle * 10.0f) % 3600);
        lv_img_set_angle(navigation_arrow, angle_tenths);
    }
}

// All filesystem operations are worker-only. Missing/partial tiles use a visible checker.
static bool load_map_pixels(uint8_t *pixels, int zoom, int x, int y)
{
    bool valid = false;
    if (y >= 0 && y < (1 << zoom)) {
        char path[96];
        snprintf(path, sizeof(path), "/sdcard/maps/%d/%d/%d", zoom, y, x);
        FILE *file = fopen(path, "rb");
        if (file) {
            valid = mapdiag::read_tile_fast(file, pixels) == mapdiag::FileStatus::Ok;
            fclose(file);
        }
    }
    if (!valid) {
        uint16_t *out = (uint16_t *)pixels;
        for (int py = 0; py < 256; ++py)
            for (int px = 0; px < 256; ++px)
                out[py * 256 + px] = ((px / 16 + py / 16) & 1) ? 0xDEFB : 0xEF7D;
    }
    return valid;
}


// Injectable IO for host tests; defaults to the actual worker-only reader.
static bool (*map_tile_reader)(uint8_t *, int, int, int) = load_map_pixels;
struct MapTileKey { int zoom, x, y; };
struct MapReadResult {
    int missing = 0;
    bool stale = false;
    bool complete = false;
    bool valid[9] = {};
    MapTileKey keys[9] = {};
};
// Ownership contract: exactly one worker owns staging, these cache flags and commits.
// UI/LVGL only reads front pixels; it may change view/visibility but never frees or
// writes front pixels. Snapshot pointers under the UI lock, then copy outside it.
// The sole worker cannot swap front while copying, so rendering/copying are both
// immutable readers. No additional tile buffers or long pixel copies under lock.
static MapTileKey front_keys[9] = {};
static bool front_valid[9] = {};
static bool last_successful_revision_valid = false;
static uint32_t last_successful_revision = 0;
static bool same_tile_key(const MapTileKey &a, const MapTileKey &b)
{
    return a.zoom == b.zoom && a.x == b.x && a.y == b.y;
}
static bool request_is_current_locked(const MapRequest &request)
{
    return request.revision == map_revision && request.revision == pending_request.revision &&
           request.zoom == pending_request.zoom && request.zoom == cur_z;
}
static MapReadResult read_requested_map(const MapRequest &request, uint8_t **staging,
                                        uint8_t *const *front)
{
    MapReadResult result;
    const int n = 1 << request.zoom;
    for (int i = 0; i < 9; ++i) {
        // lvgl_port_lock(0) is an infinite wait, NOT a try-lock. All revision
        // reads shared with UI are protected, including this per-tile check.
        if (!lvgl_port_lock(0)) { result.stale = true; return result; }
        const bool current = request_is_current_locked(request);
        lvgl_port_unlock();
        if (!current) { result.stale = true; return result; }
        result.keys[i] = {request.zoom, (request.x + i % 3 - 1 + n) % n,
                          request.y + i / 3 - 1};
        int reused = -1;
        for (int j = 0; j < 9; ++j)
            if (front_valid[j] && same_tile_key(result.keys[i], front_keys[j])) {
                reused = j; break;
            }
        if (reused >= 0) {
            memcpy(staging[i], front[reused], 256 * 256 * 2);
            result.valid[i] = true;
        } else {
            // A single filesystem read is synchronous, not interruptible. A view
            // change during it is detected before the next tile (or at commit).
            result.valid[i] = map_tile_reader(staging[i], request.zoom,
                                              result.keys[i].x, result.keys[i].y);
        }
        if (!result.valid[i]) ++result.missing;
    }
    result.complete = true;
    return result;
}

// Caller holds the UI lock. A stale/partial batch never swaps even one pointer.
static bool show_loaded_map(const MapRequest &request, uint8_t **staging, const MapReadResult &result)
{
    if (!result.complete || result.stale || !request_is_current_locked(request)) return false;
    for (int i = 0; i < 9; ++i) {
        lv_img_cache_invalidate_src(&maps[i].map_dsc);
        uint8_t *old_pixels = maps[i].map_data;
        maps[i].map_data = staging[i]; staging[i] = old_pixels;
        maps[i].map_dsc.data = maps[i].map_data;
        maps[i].x = result.keys[i].x;
        maps[i].y = result.keys[i].y;
        front_keys[i] = result.keys[i];
        front_valid[i] = result.valid[i]; // A missing checker is never cached as valid.
        lv_img_set_src(maps[i].map_obj, &maps[i].map_dsc);
        lv_obj_clear_flag(maps[i].map_obj, LV_OBJ_FLAG_HIDDEN);
    }
    displayed_z = request.zoom;
    displayed_center_x = request.x;
    displayed_center_y = request.y;
    map_display_ready = true;
    last_successful_revision = request.revision;
    last_successful_revision_valid = true;
    position_displayed_map();
    if (result.missing) lv_label_set_text(map_status, "Map tiles missing");
    else if (next_map_level(available_levels, cur_z, 1) < 0 && next_map_level(available_levels, cur_z, -1) < 0)
        lv_label_set_text(map_status, "No map zoom level");
    else lv_label_set_text(map_status, "");
    return true;
}

enum class MapProcessResult { Committed, AlreadyCurrent, Unavailable };
// One semaphore service, shared verbatim by the real worker and host tests. If
// superseded, immediately snapshot newest here: do not wait for a timer/token.
static MapProcessResult process_latest_request(uint8_t **staging)
{
    for (;;) {
        MapRequest request;
        uint8_t *front[9];
        if (!lvgl_port_lock(0)) return MapProcessResult::Unavailable;
        request = pending_request;
        if (request.zoom < 0 || !levels_ready || !available_levels) {
            lvgl_port_unlock(); return MapProcessResult::Unavailable;
        }
        if (last_successful_revision_valid && request.revision == last_successful_revision) {
            lvgl_port_unlock(); return MapProcessResult::AlreadyCurrent;
        }
        for (int i = 0; i < 9; ++i) front[i] = maps[i].map_data;
        lvgl_port_unlock();
        const MapReadResult result = read_requested_map(request, staging, front);
        if (!lvgl_port_lock(0)) return MapProcessResult::Unavailable;
        const bool committed = show_loaded_map(request, staging, result);
        lvgl_port_unlock();
        if (committed) return MapProcessResult::Committed;
    }
}

static void maps_update_task(void *)
{
    // Only compact requests/pointers on the 4096-byte task stack; pixels stay in PSRAM.
    uint8_t *staging[9];
    for (int i = 0; i < 9; ++i) {
        staging[i] = (uint8_t *)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
        configASSERT(staging[i] != nullptr);
    }
    uint32_t levels = 0;
    for (int z = 0; z <= 19; ++z) {
        char path[48]; struct stat info;
        snprintf(path, sizeof(path), "/sdcard/maps/%d", z);
        if (stat(path, &info) == 0 && S_ISDIR(info.st_mode)) levels |= 1u << z;
    }
    if (lvgl_port_lock(0)) { apply_available_levels(levels); lvgl_port_unlock(); }
    while (1) {
        if (xSemaphoreTake(map_update_semphr, portMAX_DELAY) != pdTRUE) continue;
        process_latest_request(staging);
    }
}

void satelite_info_init()
{
    for (int i = 0; i < 9; ++i) {
        maps[i].map_data = (uint8_t *)heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM);
        configASSERT(maps[i].map_data != nullptr);
        maps[i].map_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        maps[i].map_dsc.header.w = 256; maps[i].map_dsc.header.h = 256;
        maps[i].map_dsc.data_size = 256 * 256 * 2;
        maps[i].map_dsc.data = maps[i].map_data;
    }
    map_update_semphr = xSemaphoreCreateBinary();
    view_x = fix_x = longitude_world(DEFAULT_LONGITUDE);
    view_y = fix_y = latitude_world(DEFAULT_LATITUDE);
    lv_obj_update_layout(map_parent);
    lv_label_set_text(connection_label, "No fix, default: Tiananmen");
    lv_label_set_text(map_status, "Loading map levels");
    update_zoom_buttons();
    request_view_map();
    // Set up all UI state before worker publishes levels under the LVGL lock.
    xTaskCreatePinnedToCore(maps_update_task, "maps_update_task", 4096, NULL, 2, NULL, 1);
    lv_timer_create(neo_m8n_listen_cb, 100, NULL);
    lv_timer_create(qmc_5883_listen_cb, 200, NULL);
    lv_timer_create(map_request_timer, 120, NULL);
}

void satelite_info_tile_init(lv_obj_t *parent)
{
    map_parent = parent;
    lv_obj_t *top_layer = lv_layer_top();
    // Fixed viewport: floating children never define scroll bounds, including negative x/y.
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM |
                              LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(parent, map_pointer_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(parent, map_pointer_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(parent, map_pointer_cb, LV_EVENT_PRESS_LOST, NULL);
    for (int i = 0; i < 9; ++i) {
        maps[i].map_obj = lv_img_create(parent);
        lv_obj_add_flag(maps[i].map_obj, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(maps[i].map_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    connection_label = lv_label_create(top_layer);
    map_status = lv_label_create(top_layer);
    map_level_label = lv_label_create(top_layer);
    lv_obj_align(map_level_label,LV_ALIGN_TOP_RIGHT,-4,27);
    lv_obj_clear_flag(map_level_label,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(connection_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(map_status, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(connection_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(connection_label, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_align(map_status, LV_ALIGN_TOP_LEFT, 2, 27);
    navigation_arrow = lv_img_create(top_layer);
    lv_img_set_src(navigation_arrow, &navigation_arrow_img);
    lv_img_set_pivot(navigation_arrow, NAVIGATION_ARROW_SIZE_PX / 2, NAVIGATION_ARROW_SIZE_PX / 2);
    lv_img_set_antialias(navigation_arrow, true);
    lv_obj_clear_flag(navigation_arrow, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(navigation_arrow, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_HIDDEN);
    zoom_in_btn = lv_btn_create(top_layer);
    zoom_out_btn = lv_btn_create(top_layer);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *button = i == 0 ? zoom_in_btn : zoom_out_btn;
        lv_obj_set_size(button, 50, 50);
        lv_obj_add_flag(button, LV_OBJ_FLAG_FLOATING);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, i == 0 ? "+" : "-");
        lv_obj_center(label);
        lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -5, i == 0 ? -60 : -5);
        lv_obj_add_event_cb(button, zoom_btn_event_handler, LV_EVENT_CLICKED, NULL);
    }
    recenter_btn = lv_btn_create(top_layer);
    lv_obj_set_size(recenter_btn,50,50);
    lv_obj_add_flag(recenter_btn,LV_OBJ_FLAG_FLOATING);
    lv_obj_align(recenter_btn,LV_ALIGN_BOTTOM_RIGHT,-5,-115);
    lv_obj_t *recenter_label = lv_label_create(recenter_btn);
    lv_label_set_text(recenter_label,LV_SYMBOL_GPS);
    lv_obj_center(recenter_label);
    lv_obj_add_event_cb(recenter_btn,recenter_btn_event_handler,LV_EVENT_CLICKED,NULL);
    satelite_info_init();
}
