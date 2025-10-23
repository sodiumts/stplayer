#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/display.h>
#include <ff.h>
#include <lvgl.h>
#include <lvgl_zephyr.h>

#include "core/lv_obj_style_gen.h"
#include "display/lv_display.h"
#include "lv_api_map_v8.h"
#include "misc/lv_color.h"
#include "usb_mass.h"
#include "audio_playback.h"

LOG_MODULE_REGISTER(main);

#define DISP_NODE DT_NODELABEL(sh1122)
static const struct device *disp;

int main(void)
{
    setup_disk();
	int ret;
    disp = DEVICE_DT_GET(DISP_NODE);
    if(!device_is_ready(disp)) {
        LOG_ERR("Display is not ready");
        return 1;
    }
    
    ret = init_audio_playback();
    if (ret < 0) {
        return ret;
    }

    lv_obj_clean(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_white(), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "hello there");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    
    lv_refr_now(NULL);
    k_msleep(50);
    //LOG_INF("Starting opus playback");
    //ret = stream_opus("/SD:/Ado - MIRROR.opus");
    //if (ret < 0) {
    //    return ret;
    //}
    //
    while (1) {
        lv_timer_handler();  // or lv_task_handler() depending on LVGL version
        k_msleep(16);
    }

    return 0;
}
