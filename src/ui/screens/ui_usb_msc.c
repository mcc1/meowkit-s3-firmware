// USB MSC screen — pure display, no enable/disable logic here.
// usb_msc_enable() is called in ui_settings.c before navigating here.
// Physical A = Eject, B = Back — both handled exclusively in launcher.cpp.
// LVGL 8.3.11

#include "../ui.h"
#include "../../system/usb_msc.h"

lv_obj_t * ui_usb_msc;
lv_obj_t * ui_usb_msc_bg;
lv_obj_t * ui_mascot_usb_msc;
lv_obj_t * ui_usb_msc_label;
lv_obj_t * ui_usb_msc_key_prompts_bg;
lv_obj_t * ui_usb_msc_key_a_bg;
lv_obj_t * ui_usb_msc_key_b_bg;
lv_obj_t * ui_usb_msc_key_a_eject;
lv_obj_t * ui_usb_msc_key_b_back;

static lv_timer_t * s_status_timer = NULL;

// 1 Hz poll — updates label with live USB MSC state
static void _msc_status_cb(lv_timer_t * t)
{
    (void)t;
    if(!ui_usb_msc_label) return;

    if(!usb_msc_is_active()) {
        lv_label_set_text(ui_usb_msc_label, "ready — eject before removing");
        return;
    }

    unsigned long bytes = usb_msc_bytes_transferred();
    unsigned retries = 0, errors = 0; unsigned long lastcb = 0;
    usb_msc_stats(&retries, &errors, &lastcb);
    char buf[64];
    /* A big FAT32 card makes the host read tens of MB of FAT before the
     * drive opens; over Full-Speed USB that is about a minute. Say so, or
     * users leave MSC half-way through the mount. */
    if(bytes == 0) {
        lv_snprintf(buf, sizeof(buf), "connected - host mounting, wait");
    } else if(bytes < 1024UL * 1024UL) {
        lv_snprintf(buf, sizeof(buf), "active  %lu KB - wait for host", bytes / 1024UL);
    } else if(lastcb && (lv_tick_get() - (uint32_t)lastcb) > 5000) {
        lv_snprintf(buf, sizeof(buf), "%lu MB  idle %lus  r%u e%u", bytes / (1024UL * 1024UL),
                    (unsigned long)((lv_tick_get() - (uint32_t)lastcb) / 1000), retries, errors);
    } else {
        lv_snprintf(buf, sizeof(buf), "active  %lu MB  r%u e%u", bytes / (1024UL * 1024UL), retries, errors);
    }
    lv_label_set_text(ui_usb_msc_label, buf);
}

void ui_usb_msc_screen_init(void)
{
    ui_usb_msc = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_usb_msc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_usb_msc, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_usb_msc, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_usb_msc_bg = lv_img_create(ui_usb_msc);
    lv_img_set_src(ui_usb_msc_bg, &ui_img_background_png);
    lv_obj_set_width(ui_usb_msc_bg, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_bg, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_usb_msc_bg, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_usb_msc_bg, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_usb_msc_bg, LV_OBJ_FLAG_SCROLLABLE);

    ui_mascot_usb_msc = lv_img_create(ui_usb_msc);
    lv_img_set_src(ui_mascot_usb_msc, &ui_img_mascot_usb_msc_png);
    lv_obj_set_width(ui_mascot_usb_msc, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_mascot_usb_msc, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_mascot_usb_msc, 0);
    lv_obj_set_y(ui_mascot_usb_msc, 25);
    lv_obj_add_flag(ui_mascot_usb_msc, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_mascot_usb_msc, LV_OBJ_FLAG_SCROLLABLE);

    ui_usb_msc_label = lv_label_create(ui_usb_msc);
    lv_obj_set_width(ui_usb_msc_label, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_label, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_label, 19);
    lv_obj_set_y(ui_usb_msc_label, 15);
    lv_label_set_text(ui_usb_msc_label, "connecting");
    lv_obj_set_style_text_color(ui_usb_msc_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_usb_msc_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_usb_msc_label, &ui_font_name_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_usb_msc_key_prompts_bg = lv_img_create(ui_usb_msc);
    lv_img_set_src(ui_usb_msc_key_prompts_bg, &ui_img_key_prompts_bg_png);
    lv_obj_set_width(ui_usb_msc_key_prompts_bg, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_key_prompts_bg, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_key_prompts_bg, 168);
    lv_obj_set_y(ui_usb_msc_key_prompts_bg, 204);
    lv_obj_add_flag(ui_usb_msc_key_prompts_bg, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_usb_msc_key_prompts_bg, LV_OBJ_FLAG_SCROLLABLE);

    // Key images — display only, touch disabled (physical buttons via launcher)
    ui_usb_msc_key_a_bg = lv_img_create(ui_usb_msc);
    lv_img_set_src(ui_usb_msc_key_a_bg, &ui_img_key_a_png);
    lv_obj_set_width(ui_usb_msc_key_a_bg, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_key_a_bg, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_key_a_bg, 171);
    lv_obj_set_y(ui_usb_msc_key_a_bg, 207);
    lv_obj_add_flag(ui_usb_msc_key_a_bg, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_usb_msc_key_a_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    ui_usb_msc_key_b_bg = lv_img_create(ui_usb_msc);
    lv_img_set_src(ui_usb_msc_key_b_bg, &ui_img_key_b_png);
    lv_obj_set_width(ui_usb_msc_key_b_bg, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_key_b_bg, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_key_b_bg, 241);
    lv_obj_set_y(ui_usb_msc_key_b_bg, 207);
    lv_obj_add_flag(ui_usb_msc_key_b_bg, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_usb_msc_key_b_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Key labels
    ui_usb_msc_key_a_eject = lv_label_create(ui_usb_msc);
    lv_obj_set_width(ui_usb_msc_key_a_eject, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_key_a_eject, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_key_a_eject, 195);
    lv_obj_set_y(ui_usb_msc_key_a_eject, 210);
    lv_label_set_text(ui_usb_msc_key_a_eject, "Eject");
    lv_obj_set_style_text_color(ui_usb_msc_key_a_eject, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_usb_msc_key_a_eject, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_usb_msc_key_a_eject, &ui_font_name_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_usb_msc_key_b_back = lv_label_create(ui_usb_msc);
    lv_obj_set_width(ui_usb_msc_key_b_back, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_usb_msc_key_b_back, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_usb_msc_key_b_back, 265);
    lv_obj_set_y(ui_usb_msc_key_b_back, 210);
    lv_label_set_text(ui_usb_msc_key_b_back, "Back");
    lv_obj_set_style_text_color(ui_usb_msc_key_b_back, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_usb_msc_key_b_back, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_usb_msc_key_b_back, &ui_font_name_14, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Start polling timer — first tick immediately, then 1 Hz
    _msc_status_cb(NULL);
    s_status_timer = lv_timer_create(_msc_status_cb, 1000, NULL);
}

void ui_usb_msc_screen_destroy(void)
{
    if(s_status_timer) { lv_timer_del(s_status_timer); s_status_timer = NULL; }
    if(ui_usb_msc) lv_obj_del(ui_usb_msc);

    ui_usb_msc                = NULL;
    ui_usb_msc_bg             = NULL;
    ui_mascot_usb_msc         = NULL;
    ui_usb_msc_label          = NULL;
    ui_usb_msc_key_prompts_bg = NULL;
    ui_usb_msc_key_a_bg       = NULL;
    ui_usb_msc_key_b_bg       = NULL;
    ui_usb_msc_key_a_eject    = NULL;
    ui_usb_msc_key_b_back     = NULL;
}
