#include "satelite_info_tile.h"

void satelite_info_tile_init(lv_obj_t *parent)
{
    /*Create a list*/
    lv_obj_t *list = lv_list_create(parent);
    lv_obj_t *lable = lv_label_create(parent);
    lv_obj_set_style_text_font(lable, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(lable, "Satelites");
    lv_obj_align(lable, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_set_size(list, lv_pct(95), lv_pct(80));
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 30);
}
