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

#include "usb_mass.h"
#include "audio_playback.h"

LOG_MODULE_REGISTER(main);

#define DISP_NODE DT_NODELABEL(sh1122)
static const struct device *disp;

void do_write_gray_full(uint8_t gray_level) {
    const int W = 256;
    const int H = 64;

    static uint8_t buf[256 * 64];
    uint32_t bytes_total = 256*64;
    uint8_t packed = gray_level << 4;
    memset(buf, packed, bytes_total);

    struct display_buffer_descriptor desc = {
        .buf_size = (uint32_t)bytes_total,
        .width = W,
        .height = H,
        .pitch = W,
    };

    int rc = display_write(disp, 0, 0, &desc, buf);
}

int main(void)
{
	int ret;
//    disp = DEVICE_DT_GET(DISP_NODE);
//    if(!device_is_ready(disp)) {
//        LOG_ERR("Display is not ready");
//        return 1;
//    }
//
//    do_write_gray_full(0b1111);
    //display_set_contrast(disp, 128);
    //for (int i = 15; i >= 0; i--) {
    //    do_write_gray_full(i);
    //    k_sleep(K_SECONDS(1));
    //}

    //display_blanking_off(disp);
    //
    //k_sleep(K_SECONDS(2));
   // display_blanking_on(disp);
	
	setup_disk();

//    ret = setup_mass();
//    if (ret != 0) {
//        return ret;
//    }
    ret = init_audio_playback();
    if (ret < 0) {
        return ret;
    }

    LOG_INF("Starting opus playback");
    ret = stream_opus("/SD:/Ado - MIRROR.opus");
    if (ret < 0) {
        return ret;
    }

    return 0;
}
