#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/usb/usb_device.h>
#include <ff.h>

#include "usb_mass.h"
#include "audio_playback.h"

LOG_MODULE_REGISTER(main);

int main(void)
{
	int ret;
	
	setup_disk();

    ret = setup_mass();
    if (ret != 0) {
        return ret;
    }
//    ret = init_audio_playback();
//    if (ret < 0) {
//        return ret;
//    }
//
//    LOG_INF("Starting opus playback");
//    ret = stream_opus("/NAND:/ADO-MI~1.OGX");
//    if (ret < 0) {
//        return ret;
//    }

    return 0;
}
