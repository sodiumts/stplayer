#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/adc.h>
#include <ff.h>

#include <lvgl.h>
#include <lvgl_zephyr.h>
#include <core/lv_obj_pos.h>
#include <core/lv_obj_style_gen.h>
#include <display/lv_display.h>
#include <misc/lv_area.h>
#include <misc/lv_color.h>

#include "usb_mass.h"
#include "audio_playback.h"

LOG_MODULE_REGISTER(main);

#define DISP_NODE DT_NODELABEL(sh1122)
static const struct device *disp;

static const struct adc_dt_spec adc_chan =ADC_DT_SPEC_GET(DT_PATH(zephyr_user));

#define AUDIO_THREAD_PRIO 1
#define INPUT_THREAD_PRIO 2

K_PIPE_DEFINE(pipe, 256, 4);

uint16_t read_potentiometer(const struct adc_dt_spec *adc_cha)
{
    int ret;
    uint16_t buf;
    struct adc_sequence sequence = {
        .buffer = &buf,
        .buffer_size = sizeof(buf),
        .oversampling = 4
    };

    if (!device_is_ready(adc_cha->dev)) {
        printk("ADC device not ready\n");
        return -1;
    }

    ret = adc_sequence_init_dt(adc_cha, &sequence);
    if (ret != 0) {
        printk("Failed to initialize ADC sequence: %d\n", ret);
        return ret;
    }

    ret = adc_read(adc_cha->dev, &sequence);
    if (ret != 0) {
        printk("Failed to read ADC: %d\n", ret);
        return ret;
    }

    return buf;
} 

int main(void)
{
    setup_disk();
	int ret;
    disp = DEVICE_DT_GET(DISP_NODE);
    if(!device_is_ready(disp)) {
        LOG_ERR("Display is not ready");
        return 1;
    }

    display_blanking_off(disp);
    
    lv_obj_clean(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_screen_active(), lv_color_white(), LV_PART_MAIN);

    lv_obj_t *list = lv_list_create(lv_screen_active());
    lv_obj_set_style_bg_color(list, lv_color_black(), 0);
    lv_obj_set_size(list, 256,64);

    populate_list_with_files(list);
    
    lv_refr_now(NULL);
    k_msleep(50);
    ret = adc_channel_setup_dt(&adc_chan);
    if (ret < 0) {
        return ret;
    }
     
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    audio_thread_msg audioMessage;

    audioMessage.msg_type = PLAY;
    strncpy(audioMessage.song_path, "/SD:/Ado - MIRROR.opus", sizeof(audioMessage.song_path));
    ret = k_pipe_write(&pipe, (uint8_t *) &audioMessage, sizeof(audioMessage), K_FOREVER);
    if (ret > 0) {
        LOG_INF("Wrote %d bytes", ret);
    } else if (ret != -EAGAIN) {
        LOG_ERR("Write error %d", ret);
    }
    
    while(1) {
        uint16_t volume = read_potentiometer(&adc_chan);
        audioMessage.msg_type = VOL;
        audioMessage.volume = volume;
        int ret = k_pipe_write(&pipe, (uint8_t *) &audioMessage, sizeof(audioMessage), K_FOREVER);
        if (ret > 0) {
        } else if (ret != -EAGAIN) {
            LOG_ERR("Write error %d", ret);
        }
        lv_timer_handler();

        lv_label_set_text_fmt(label, "%d", volume);
    }
    return 0;
}

K_THREAD_DEFINE(audio_tid, 20000, audio_handler_thread, &pipe, NULL, NULL, AUDIO_THREAD_PRIO, 0, 200);
