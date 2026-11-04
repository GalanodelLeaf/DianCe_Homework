#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <algorithm>
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
    assert(ticks == 0);
    const size_t length = std::min((size_t)size, uart_input.size());
    memcpy(buffer, uart_input.data(), length);
    uart_input.erase(0, length);
    return (int)length;
}
extern "C" bool esp_qmc5883_port_read_heading(float *heading)
{
    *heading = test_heading; return true;
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
static void flush(lv_disp_drv_t *driver, const lv_area_t *, lv_color_t *) { lv_disp_flush_ready(driver); }
static uint8_t *staging[9];
static unsigned tile_reads = 0;
static bool mock_tiles_missing = false;
static std::vector<MapTileKey> read_keys;
static MapTileKey fail_key = {-1, -1, -1};
static void (*reader_hook)() = nullptr;
static void fill_mock_tile(uint8_t *pixels, int z, int x, int y, bool valid)
{
    // Repeated full (z,x,y) tuple distinguishes every neighbor, wrap and layer.
    uint32_t words[4] = {(uint32_t)z, (uint32_t)x, (uint32_t)y, valid ? 0x1234ABCDu : 0xBAD0BAD0u};
    for (size_t i=0; i<256*256*2; i+=sizeof(words)) memcpy(pixels+i,words,sizeof(words));
}
static bool mock_tile_reader(uint8_t *pixels, int zoom, int x, int y)
{
    assert(test_lvgl_lock_depth == 0); // No IO or tile pixels under the UI lock.
    assert(zoom >= 0 && zoom <= 19 && x >= 0 && x < (1 << zoom));
    ++tile_reads; read_keys.push_back({zoom,x,y});
    const bool valid = !mock_tiles_missing && y >= 0 && y < (1 << zoom) &&
                       !same_tile_key({zoom,x,y},fail_key);
    fill_mock_tile(pixels,zoom,x,y,valid);
    if (reader_hook) reader_hook();
    return valid;
}
static void assert_front_pixels()
{
    std::vector<uint8_t> expected(256*256*2);
    for (int i=0;i<9;++i) {
        const int n=1<<pending_request.zoom;
        const MapTileKey key={pending_request.zoom,(pending_request.x+i%3-1+n)%n,pending_request.y+i/3-1};
        assert(same_tile_key(key,front_keys[i]));
        assert(maps[i].x==key.x && maps[i].y==key.y);
        fill_mock_tile(expected.data(),key.zoom,key.x,key.y,front_valid[i]);
        assert(memcmp(maps[i].map_data,expected.data(),expected.size())==0);
        assert(maps[i].map_dsc.data==maps[i].map_data);
    }
}
static void render_requested_map()
{
    const auto result = process_latest_request(staging);
    assert(result==MapProcessResult::Committed || result==MapProcessResult::AlreadyCurrent);
    assert_front_pixels();
    lv_obj_update_layout(lv_layer_top());
}
static void assert_marker(int x, int y, bool visible)
{
    lv_obj_update_layout(lv_scr_act()); lv_obj_update_layout(lv_layer_top());
    assert(lv_obj_has_flag(navigation_arrow, LV_OBJ_FLAG_HIDDEN) == !visible);
    if (visible) {
        lv_area_t coords;
        lv_obj_get_coords(navigation_arrow, &coords);
        assert(coords.x1 + NAVIGATION_ARROW_SIZE_PX / 2 == x);
        assert(coords.y1 + NAVIGATION_ARROW_SIZE_PX / 2 == y);
    }
}
static lv_indev_t *pointer;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_REL;
static void pointer_read(lv_indev_drv_t *, lv_indev_data_t *data)
{
    data->point = pointer_point; data->state = pointer_state;
}
static void pointer_sample(int x, int y, lv_indev_state_t state)
{
    pointer_point = {(lv_coord_t)x, (lv_coord_t)y}; pointer_state = state;
    lv_tick_inc(20);
    lv_indev_read_timer_cb(pointer->driver->read_timer);
}
static void drag(int x1, int y1, int x2, int y2)
{
    pointer_sample(x1, y1, LV_INDEV_STATE_PR);
    for (int step = 1; step <= 8; ++step)
        pointer_sample(x1+(x2-x1)*step/8, y1+(y2-y1)*step/8, LV_INDEV_STATE_PR);
    pointer_sample(x2, y2, LV_INDEV_STATE_REL);
    lv_obj_update_layout(map_parent);
}
static void click(lv_obj_t *button)
{
    lv_obj_update_layout(lv_layer_top());
    lv_area_t area; lv_obj_get_coords(button, &area);
    const int x=(area.x1+area.x2)/2, y=(area.y1+area.y2)/2;
    pointer_sample(x, y, LV_INDEV_STATE_PR); pointer_sample(x, y, LV_INDEV_STATE_REL);
}
static void move_fix_to(int x, int y)
{
    const double scale = 256.0 * (1u << cur_z);
    view_x = wrap_world(fix_x-(x-lv_obj_get_content_width(map_parent)/2)/scale);
    view_y = clamp_world_y(fix_y-(y-lv_obj_get_content_height(map_parent)/2)/scale);
    request_view_map(); lv_obj_update_layout(map_parent);
}
static void test_simple_map_controls()
{
    uint32_t count=0; bool plus=false, minus=false, recenter=false;
    std::vector<lv_obj_t *> pending{lv_layer_top()};
    while (!pending.empty()) {
        lv_obj_t *obj=pending.back(); pending.pop_back();
        if (lv_obj_check_type(obj, &lv_label_class)) {
            const char *text=lv_label_get_text(obj);
            assert(strcmp(text,"CAL") != 0 && strcmp(text,"Cancel") != 0 && strcmp(text,"Hold RESET") != 0);
            assert(strstr(text,"Compass uncalibrated") == nullptr && strstr(text,"CAL rejected") == nullptr);
            assert(strstr(text,"Step") == nullptr);
        }
        if (lv_obj_get_parent(obj)==lv_layer_top() && lv_obj_check_type(obj,&lv_btn_class)) {
            ++count; assert(lv_obj_get_child_cnt(obj)==1);
            lv_obj_t *label=lv_obj_get_child(obj,0); assert(lv_obj_check_type(label,&lv_label_class));
            const char *text=lv_label_get_text(label);
            assert(strcmp(text,"+")==0 || strcmp(text,"-")==0 || strcmp(text,LV_SYMBOL_GPS)==0);
            if (strcmp(text,"+")==0) { assert(obj==zoom_in_btn); plus=true; }
            else if(strcmp(text,"-")==0) { assert(obj==zoom_out_btn); minus=true; }
            else { assert(obj==recenter_btn); recenter=true; }
        }
        for (uint32_t i=0; i<lv_obj_get_child_cnt(obj); ++i) pending.push_back(lv_obj_get_child(obj,i));
    }
    assert(count==3 && plus && minus && recenter);
    puts("PASS: +/- and recenter buttons, no CAL controls/status.");
}
static void test_map_navigation()
{
    lv_init();
    static lv_color_t pixels[320*10]; static lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer,pixels,NULL,320*10);
    static lv_disp_drv_t driver; lv_disp_drv_init(&driver);
    driver.hor_res=320; driver.ver_res=480; driver.draw_buf=&buffer; driver.flush_cb=flush;
    lv_disp_drv_register(&driver);
    satelite_info_tile_init(lv_scr_act()); lv_obj_update_layout(lv_layer_top());
    assert(!has_real_fix && !levels_ready);
    assert(lv_obj_has_state(zoom_in_btn,LV_STATE_DISABLED) && lv_obj_has_state(zoom_out_btn,LV_STATE_DISABLED));
    assert(pending_request.x==(int)lon_to_x(116.3975,cur_z));
    assert(pending_request.y==(int)lat_to_y(39.9087,cur_z));
    for (auto &data: staging) data=(uint8_t *)calloc(256*256,2);
    map_tile_reader=mock_tile_reader; // No real SD IO; worker task is stubbed.
    apply_available_levels((1u<<12)|(1u<<14)|(1u<<16)); render_requested_map();
    assert_marker(160,240,true);
    static lv_indev_drv_t input; lv_indev_drv_init(&input);
    input.type=LV_INDEV_TYPE_POINTER; input.read_cb=pointer_read;
    pointer=lv_indev_drv_register(&input);
    const auto valid=nmea("GNRMC,120000.00,A,4000.0,N,11700.0,E,0,0,031126,,");
    uart_input=valid; neo_m8n_listen_cb(NULL);
    assert(has_real_fix && ever_real_fix && !manual_view && view_x==fix_x && view_y==fix_y);
    render_requested_map(); assert_marker(160,240,true);
    const double start_x=view_x, start_y=view_y; const int start_tile=maps[4].x;
    drag(160,240,60,140); // Actual PRESS/MOVE/RELEASE over marker to upper left.
    assert(manual_view && view_x>start_x && view_y>start_y);
    assert(fabs((view_x-start_x)*(256.0*(1u<<cur_z))-100)<1e-6);
    assert_marker(60,140,true);
    const double released_x=view_x, released_y=view_y;
    for (int i=0;i<10;++i) { lv_tick_inc(30); lv_timer_handler(); }
    assert(view_x==released_x && view_y==released_y);
    assert(lv_obj_get_scroll_x(map_parent)==0 && lv_obj_get_scroll_y(map_parent)==0);
    drag(60,140,160,240);
    assert(fabs(view_x-start_x)<1e-12 && fabs(view_y-start_y)<1e-12); assert_marker(160,240,true);
    for (int i=0;i<8;++i) drag(160,240,60,140);
    render_requested_map(); assert(maps[4].x>start_tile+1);
    assert(fabs((view_x-start_x)*(256.0*(1u<<cur_z))-800)<1e-6); assert_marker(-640,-560,false);
    for (int i=0;i<8;++i) drag(60,140,160,240);
    render_requested_map(); assert_marker(160,240,true);
    // Non-clickable overlays must not eat pointer presses; pressing the map and
    // crossing a zoom button retains the map press rather than activating zoom.
    const int z_before_drag=cur_z;
    drag(160,350,285,395);
    assert(cur_z==z_before_drag);
    drag(285,350,160,305); // Starts outside the button, restores the same delta.
    assert(fabs(view_x-start_x)<1e-12 && fabs(view_y-start_y)<1e-12);
    drag(100,35,200,135); // Starts over the status label.
    assert(fabs((view_x-start_x)*(256.0*(1u<<cur_z))+100)<1e-6);
    drag(200,135,100,35);
    assert(fabs(view_x-start_x)<1e-12 && fabs(view_y-start_y)<1e-12);
    render_requested_map(); assert_marker(160,240,true);
    puts("PASS: real LVGL pointer upper-left/reverse, marker/label hit-through, cross-button lock, >500px neighbors, no rebound.");
    const lv_point_t positions[]={{95,160},{0,240},{-1,240},{0,240},{319,240},{320,240},{319,240},
        {160,0},{160,-1},{160,0},{160,479},{160,480},{160,479},{-100,-100},{160,240}};
    for (const auto &p:positions) {
        move_fix_to(p.x,p.y); assert_marker(p.x,p.y,p.x>=0 && p.x<320 && p.y>=0 && p.y<480);
    }
    move_fix_to(95,160);
    for (float heading:{0.0f,90.0f,180.0f,270.0f,359.9f}) {
        test_heading=heading; qmc_5883_listen_cb(NULL); assert_marker(95,160,true);
        assert(lv_img_get_angle(navigation_arrow)==lroundf(heading*10));
    }
    const double anchor_x=view_x, anchor_y=view_y;
    const MapRequest old=pending_request;
    click(zoom_in_btn); assert(cur_z==14 && view_x==anchor_x && view_y==anchor_y);
    click(zoom_in_btn); assert(cur_z==16 && lv_obj_has_state(zoom_in_btn,LV_STATE_DISABLED));
    uint8_t *visible_pixels=maps[4].map_data;
    assert(!show_loaded_map(old,staging,MapReadResult{}) && maps[4].map_data==visible_pixels);
    const MapRequest at16=pending_request;
    click(zoom_out_btn); assert(cur_z==14 && !show_loaded_map(at16,staging,MapReadResult{}));
    click(zoom_out_btn); assert(cur_z==12 && view_x==anchor_x && view_y==anchor_y);
    render_requested_map(); assert_marker(95,160,true);
    puts("PASS: pointer +/- sparse zoom, center anchor, rapid zoom stale requests rejected.");
    const auto revision=map_revision; const unsigned reads=tile_reads;
    for (int i=0;i<10;++i) { uart_input=valid; neo_m8n_listen_cb(NULL); }
    assert(view_x==anchor_x && view_y==anchor_y && map_revision==revision);
    uart_input=nmea("GNRMC,120000.00,A,4000.06,N,11700.06,E,0,0,031126,,"); neo_m8n_listen_cb(NULL);
    assert(has_real_fix && view_x==anchor_x && view_y==anchor_y && map_revision==revision);
    assert_marker(98,156,true);
    lv_tick_inc(2999); neo_m8n_listen_cb(NULL); assert(has_real_fix);
    lv_tick_inc(1); neo_m8n_listen_cb(NULL); assert(!has_real_fix);
    assert(view_x==anchor_x && view_y==anchor_y && map_revision==revision); assert_marker(98,156,true);
    uart_input=valid; neo_m8n_listen_cb(NULL);
    uart_input=nmea("GNRMC,120001.00,V,,,,,,,031126,,,N"); neo_m8n_listen_cb(NULL);
    assert(!has_real_fix && view_x==anchor_x && view_y==anchor_y && tile_reads==reads);
    ever_real_fix=false; uart_input=valid; neo_m8n_listen_cb(NULL);
    assert(view_x==anchor_x && view_y==anchor_y); assert_marker(95,160,true);
    puts("PASS: first fix policy, GPS projection, no IO per fix, timeout/V keeps manual viewport.");
    assert(next_map_level(1u<<12,12,1)==-1 && next_map_level(1u<<12,12,-1)==-1);
    apply_available_levels(1u<<12);
    assert(lv_obj_has_state(zoom_in_btn,LV_STATE_DISABLED) && lv_obj_has_state(zoom_out_btn,LV_STATE_DISABLED));
    assert(strcmp(lv_label_get_text(map_status),"No map zoom level")==0);
    mock_tiles_missing=true;
    view_x=wrap_world(view_x+10.0/(1<<cur_z)); request_view_map(); render_requested_map();
    assert(strcmp(lv_label_get_text(map_status),"Map tiles missing")==0 && has_real_fix);
    mock_tiles_missing=false; apply_available_levels(0);
    assert(strcmp(lv_label_get_text(map_status),"No map zoom level")==0);
    assert(lv_obj_has_state(zoom_in_btn,LV_STATE_DISABLED));
    apply_available_levels((1u<<0)|(1u<<19)); assert(cur_z==19);
    assert(latitude_world(90)==latitude_world(MERCATOR_MAX_LAT));
    assert(latitude_world(-90)==latitude_world(-MERCATOR_MAX_LAT));
    assert(longitude_world(180)==longitude_world(-180));
    view_x=longitude_world(179.99999); view_y=0.5;
    fix_x=longitude_world(-179.99999); fix_y=view_y;
    request_view_map(); render_requested_map(); assert_marker(167,240,true);
    drag(160,240,60,140); assert(view_x<0.001 && view_y>0.5);
    request_view_map(); render_requested_map();
    assert(pending_request.x>=0 && pending_request.x<(1<<cur_z));
    view_y=0; request_view_map(); drag(60,140,160,240); assert(view_y==0);
    view_y=1-1e-12; request_view_map(); drag(160,240,60,140); assert(view_y==1-1e-12);
    click(zoom_out_btn); assert(cur_z==0); render_requested_map();
    assert(maps[1].px-maps[0].px==256 && maps[2].px-maps[1].px==256);
    test_simple_map_controls();
    // Same-screen nested parent: content origin is not necessarily display (0,0).
    lv_obj_t *screen=map_parent;
    lv_obj_t *nested=lv_obj_create(screen);
    lv_obj_remove_style_all(nested);
    lv_obj_set_pos(nested,20,60); lv_obj_set_size(nested,280,380);
    lv_obj_clear_flag(nested,LV_OBJ_FLAG_SCROLLABLE);
    for (auto &tile:maps) lv_obj_set_parent(tile.map_obj,nested);
    map_parent=nested;
    view_x=fix_x; view_y=fix_y; request_view_map();
    lv_obj_update_layout(screen); navigation_arrow_update_position();
    assert_marker(160,250,true);
    map_parent=screen;
    for (auto &tile:maps) lv_obj_set_parent(tile.map_obj,screen);
    lv_obj_del(nested); request_view_map();
    lv_refr_now(NULL);
    puts("PASS: missing levels/tiles, z0/z19, antimeridian, latitude clamp, offset parent, real renderer.");
}
static void select_test_view(int z, int x, int y)
{
    cur_z=z; view_x=(x+0.5)/(1<<z); view_y=(y+0.5)/(1<<z);
    request_view_map();
}
static void expect_reads(unsigned expected)
{
    const unsigned before=tile_reads;
    render_requested_map();
    const size_t bytes=(tile_reads-before)*(size_t)(256*256*2);
    assert(tile_reads-before==expected && bytes==expected*(size_t)131072);
    printf("PASS: map request reads=%u bytes=%zu.\n",expected,bytes);
}
static unsigned stale_at=0, stale_seen=0;
static int stale_new_z=12;
static uint8_t *before_stale_front[9];
static uint32_t before_stale_words[9];
static void inject_newest_view()
{
    for (int i=0;i<9;++i) {
        assert(maps[i].map_data==before_stale_front[i]);
        assert(memcmp(maps[i].map_data,&before_stale_words[i],sizeof(uint32_t))==0);
    }
    if (++stale_seen==stale_at) select_test_view(stale_new_z,200,210);
}
static void test_map_performance()
{
    available_levels=(1u<<0)|(1u<<1)|(1u<<12)|(1u<<14)|(1u<<16);
    levels_ready=true; mock_tiles_missing=false; fail_key={-1,-1,-1};
    // Cold cache only; production never clears a valid front on zoom/hide.
    for (bool &valid:front_valid) valid=false;
    select_test_view(12,100,100); expect_reads(9);
    const uint32_t revision=map_revision;
    uint8_t *unchanged=maps[4].map_data;
    expect_reads(0); expect_reads(0);
    assert(map_revision==revision && maps[4].map_data==unchanged);
    select_test_view(12,101,100); expect_reads(3); // Six immutable front tiles reused.
    select_test_view(12,101,101); expect_reads(3);
    select_test_view(12,102,102); expect_reads(5);
    select_test_view(12,150,160); expect_reads(9);
    select_test_view(14,150,160); expect_reads(9); // Same x/y never aliases old zoom.
    select_test_view(12,4095,100); expect_reads(9);
    select_test_view(12,0,100); expect_reads(3); // Wrapped neighbors still match keys.
    select_test_view(12,1,100); expect_reads(3);

    fail_key={12,301,300};
    select_test_view(12,300,300); expect_reads(9);
    assert(!front_valid[5]);
    fail_key={-1,-1,-1};
    expect_reads(0); // Same successful revision, even if it displayed a checker.
    select_test_view(12,301,300); expect_reads(4); // Three new + one failed retry.
    for (bool valid:front_valid) assert(valid);
    select_test_view(12,300,300); expect_reads(3);
    expect_reads(0);
    puts("PASS: failed tile is retryable on new revision, never a permanent checker cache.");

    // z1/z0 duplicate x keys remain independent display slots; invalid latitude
    // checkers are never reusable. Every commit records all nine exact keys.
    select_test_view(1,0,1); expect_reads(9);
    select_test_view(1,1,1); expect_reads(3); // Invalid bottom row retried only.
    assert(maps[0].x==maps[2].x && maps[0].map_data!=maps[2].map_data);
    select_test_view(0,0,0); expect_reads(9); expect_reads(0);
    // Explicit new revision of the same keys exercises failed-row retries.
    pending_request.revision=++map_revision;
    expect_reads(6);

    for (unsigned k:{1u,4u,9u}) {
        select_test_view(12,400,410); expect_reads(9);
        for (int i=0;i<9;++i) {
            before_stale_front[i]=maps[i].map_data;
            memcpy(&before_stale_words[i],maps[i].map_data,sizeof(uint32_t));
        }
        select_test_view(12,100,110);
        stale_at=k; stale_seen=0; stale_new_z=(k==4 ? 14 : 12);
        read_keys.clear(); reader_hook=inject_newest_view;
        const unsigned before=tile_reads;
        assert(process_latest_request(staging)==MapProcessResult::Committed);
        reader_hook=nullptr;
        assert(tile_reads-before==k+9 && read_keys.size()==k+9);
        for (unsigned i=0;i<k;++i)
            assert(same_tile_key(read_keys[i],{12,99+(int)(i%3),109+(int)(i/3)}));
        for (unsigned i=0;i<9;++i)
            assert(same_tile_key(read_keys[k+i],{stale_new_z,199+(int)(i%3),209+(int)(i/3)}));
        assert(displayed_z==stale_new_z && displayed_center_x==200 && displayed_center_y==210);
        assert(last_successful_revision==pending_request.revision);
        assert_front_pixels(); expect_reads(0);
        printf("PASS: stale at reader %u stops old batch immediately; newest commits in same service.\n",k);
    }
    assert(test_lvgl_lock_depth==0);
    puts("PASS: shared production worker helper, full-key pixels, 0/3/5/9 reads and bytes, wrap, zoom, low-z duplicates, no partial stale swap.");
}

static void test_recenter_control()
{
    available_levels=(1u<<12)|(1u<<14)|(1u<<16); levels_ready=true;
    cur_z=14; has_real_fix=true; ever_real_fix=true; manual_view=true;
    fix_x=longitude_world(116.4); fix_y=latitude_world(39.9);
    view_x=wrap_world(fix_x+0.01); view_y=clamp_world_y(fix_y+0.01);
    request_view_map(); navigation_arrow_update_position();
    assert(lv_obj_has_flag(navigation_arrow,LV_OBJ_FLAG_HIDDEN));
    click(recenter_btn);
    assert(cur_z==14 && view_x==fix_x && view_y==fix_y && manual_view);
    assert_marker(160,240,true); render_requested_map();
    const auto reads=tile_reads, revision=map_revision;
    click(recenter_btn); render_requested_map();
    assert(tile_reads==reads && map_revision==revision);
    // Recenter retains a last-known fix after GPS loss, rather than defaulting.
    has_real_fix=false;
    view_x=wrap_world(fix_x+0.01); request_view_map();
    click(recenter_btn); assert(cur_z==14 && view_x==fix_x && view_y==fix_y);
    assert_marker(160,240,true);
    // Before any GPS fix, the arrow refers to the configured default position.
    ever_real_fix=false; fix_x=longitude_world(DEFAULT_LONGITUDE); fix_y=latitude_world(DEFAULT_LATITUDE);
    view_x=0.1; view_y=0.2; request_view_map(); click(recenter_btn);
    assert(view_x==fix_x && view_y==fix_y && cur_z==14); assert_marker(160,240,true);
    // It is one-shot: a subsequent GPS update moves the marker, not the viewport.
    const double centered_x=view_x, centered_y=view_y;
    uart_input=nmea("GNRMC,120000.00,A,3954.6000,N,11624.0000,E,0,0,031126,,");
    neo_m8n_listen_cb(nullptr);
    assert(has_real_fix && view_x==centered_x && view_y==centered_y);
    // Dragging through the new button retains the map's locked press.
    const double before_drag=view_x;
    drag(160,290,285,335);
    assert(view_x!=before_drag && cur_z==14);
    click(recenter_btn); assert(view_x==fix_x && view_y==fix_y); assert_marker(160,240,true);
    lv_refr_now(nullptr);
    puts("PASS: real pointer recenter restores offscreen marker, preserves zoom, handles default/last fix, repeats with zero IO, no follow mode or cross-button activation.");
}

int main() { test_gps_parser(); test_map_navigation(); test_map_performance(); test_recenter_control(); return 0; }
