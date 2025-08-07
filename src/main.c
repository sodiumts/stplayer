#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/usb/usb_device.h>

#include "usb_mass.h"

#define I2S_DEV DT_NODELABEL(i2s3)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

LOG_MODULE_REGISTER(main);

#define SAMPLE_NO 64
#define NUM_BLOCKS 20
#define CHANNELS 2
#define BLOCK_SIZE (SAMPLE_NO * CHANNELS * sizeof(int16_t))

static char __aligned(WB_UP(32)) _k_mem_slab_buf_tx_0_mem_slab[(NUM_BLOCKS) * WB_UP(BLOCK_SIZE)];
static STRUCT_SECTION_ITERABLE(k_mem_slab, tx_0_mem_slab) = Z_MEM_SLAB_INITIALIZER(tx_0_mem_slab, _k_mem_slab_buf_tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS);

static void fill_buf_continuous(int16_t *tx_block)
{
    for (int i = 0; i < SAMPLE_NO; i++) {
        // Generate 1kHz square wave
        int16_t value = (i < SAMPLE_NO/2) ? 32767 : -32768;
        tx_block[2 * i] = value;     // Left channel
        tx_block[2 * i + 1] = value;  // Right channel
    }
}

int main(void)
{
	int ret;

	ret = usb_enable(NULL);
	if (ret < 0) {
		LOG_ERR("Failed to enable USB\n");
        return 1;
	}
	setup_disk();

	LOG_INF("The device is put in USB mass storage mode.\n");

    if (!device_is_ready(i2s_dev)) {
        LOG_ERR("I2S device not ready\n");
        return 1;
    }
    
    struct i2s_config cfg = {
        .word_size       = 16,
        .channels        = CHANNELS,
        .format          = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_DATA_ORDER_MSB,
        .options         = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq  = 44100,
        .block_size      = BLOCK_SIZE,
        .timeout         = 2000,
        .mem_slab = &tx_0_mem_slab,
    };
    printk("Before config\n");
    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret) {
        printk("I2S config failed: %d\n", ret);
        return 1;
    }

    printk("HEREEEEEEEEE\n");

    void *tx_blocks[NUM_BLOCKS];
    for (int i = 0; i < NUM_BLOCKS; i++) {
    ret = k_mem_slab_alloc(&tx_0_mem_slab, &tx_blocks[i], K_FOREVER);
        if (ret < 0) {
            LOG_ERR("TX block alloc failed: %d\n", ret);
            return ret;
        }
        fill_buf_continuous(tx_blocks[i]);
    }


    ret = i2s_write(i2s_dev, tx_blocks[0], BLOCK_SIZE);
    if (ret) {
        printk("Could not do initial write: %d\n", ret);
        return 1;
    }

    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret) {
        printk("I2S trigger start failed: %d\n", ret);
        return 1;
    }

    for (int i = 1; i < NUM_BLOCKS; i++) {
        ret = i2s_write(i2s_dev, tx_blocks[i], BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("Initial write failed: %d", ret);
        }
    }

    while (1) {
        void *block;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("Buffer alloc failed: %d", ret);
            continue;
        }

        fill_buf_continuous(block);

        ret = i2s_write(i2s_dev, block, BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("I2S write failed: %d", ret);
            k_mem_slab_free(&tx_0_mem_slab, &block);
        }
    }
    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    
    if (ret < 0) {
        printf("Could not trigger I2S tx\n");
        return ret;
    }
	return 0;
}
