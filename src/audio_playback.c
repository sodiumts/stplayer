#include "audio_playback.h"

#include <zephyr/logging/log.h>
#include <opus.h>
#include <zephyr/drivers/i2s.h>

LOG_MODULE_REGISTER(audio_playback, LOG_LEVEL_DBG);

static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

static uint8_t opus_packet[500];
static opus_state_t op_state;
static struct fs_file_t filep;
OpusDecoder *decoder;

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS, 32);

static void apply_volume_int16(int16_t *samples, size_t nsamples, uint8_t vol_percent)
{
    if (vol_percent == 100) {
        return;
    }
    if (vol_percent == 0) {
        memset(samples, 0, nsamples * sizeof(int16_t));
        return;
    }

    uint32_t scale = (vol_percent * 256U + 50U) / 100U;

    for (size_t i = 0; i < nsamples; i++) {
        int32_t s = samples[i];
        int32_t tmp = s * (int32_t)scale;

        tmp += (tmp >= 0) ? 128 : -128;
        tmp >>= 8;

        if (tmp > INT16_MAX) {
            tmp = INT16_MAX;
        } else if (tmp < INT16_MIN) {
            tmp = INT16_MIN;
        }

        samples[i] = (int16_t)tmp;
    }
}

static int start_i2s_dma() {
    LOG_INF("Pre-filling DMA buffers");
    int ret;
    
    for (int i = 0; i < 2; i++){
        void *init_block;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &init_block, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("TX block alloc failed: %d\n", ret);
            return ret;
        }
        memset(init_block, 0, BLOCK_SIZE);

        ret = i2s_write(i2s_dev, init_block, BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("i2s_write initial block failed: %d", ret);
            k_mem_slab_free(&tx_0_mem_slab, &init_block);
        }
    }

    LOG_INF("Starting DMAs");
    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret) {
        LOG_ERR("I2S trigger start failed: %d", ret);
        return -1;
    }
    return 0;
}

static int stop_i2s_dma() {
    int ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (ret < 0) {
        LOG_ERR("Failed to stop i2s with drain: %d", ret);
        return ret;
    }
    return 0;
}

int stream_opus(const char *path) {
    int rc;
    rc = start_i2s_dma();
    fs_file_t_init(&filep);

    if ((rc = fs_open(&filep, path, FS_O_READ)) < 0) {
        LOG_ERR("fs_open failed: %d", rc);
        return rc;
    }

    opus_state_init(&op_state);

    uint16_t discard_cnt = opus_verify_header(&filep, &op_state); 
    if (discard_cnt < 0) {
        goto cleanup;
    }
    
    discard_cnt *= 2;
    
    uint16_t packet_size;
    while(1) {
        void * block = NULL;
        int rcf = opus_get_packet(&op_state, opus_packet, &packet_size, &filep);
        // Failed
        if (rcf != OP_OK && rcf != OP_DONE)
            goto cleanup;

        rc = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
        if (rc < 0) {
            LOG_ERR("Block allocation failed: %d", rc);
            break;
        }
        // Returns count of decoded samples
        int oprc = opus_decode(decoder, opus_packet, packet_size, block, SAMPLE_NO, 0);
        if (oprc < 0) {
            LOG_ERR("Opus decode failed: %d", oprc);
            goto cleanup;
        }

        if (discard_cnt > 0) {
            if (discard_cnt > SAMPLE_NO) {
                LOG_ERR("Discard count larger than one decoded buffer: %d vs %d", discard_cnt, SAMPLE_NO);
                goto cleanup;
            }

            memmove((int16_t *) block, (int16_t *) block + discard_cnt, (oprc - discard_cnt)* sizeof(int16_t));
            oprc -= discard_cnt;
            discard_cnt = 0;
        }

        rc = i2s_write(i2s_dev, block, BLOCK_SIZE);
        if (rc < 0) {
            LOG_ERR("i2s_write failed: %d", rc);
            k_mem_slab_free(&tx_0_mem_slab, &block);
            break;
        }



        // Done with file
        if (rcf == OP_DONE) {
            LOG_INF("Done with file");
            goto cleanup;
        }
    }
  cleanup:
    fs_close(&filep);
    stop_i2s_dma();
    return rc;
}

static int configure_i2s() {
    int ret;
    if (!device_is_ready(i2s_dev)) {
        LOG_ERR("I2S device not ready\n");
        return -1;
    }
    
    struct i2s_config cfg = {
        .word_size       = 16,
        .channels        = CHANNELS,
        .format          = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_DATA_ORDER_MSB,
        .options         = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq  = 48000,
        .block_size      = BLOCK_SIZE,
        .timeout         = 2000,
        .mem_slab = &tx_0_mem_slab,
    };

    LOG_INF("Configuring I2S");
    
    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret) {
        printk("I2S config failed: %d\n", ret);
        return -1;
    }

    return 0;
}

int init_audio_playback() { 
    int ret;
    decoder = opus_decoder_create(48000, CHANNELS, &ret);
    if (ret != OPUS_OK) {
        LOG_ERR("Failed to create decoder: %d", ret);
        return ret;
    }

    int size = opus_decoder_get_size(CHANNELS);
    LOG_INF("Opus decoder size: %d", size);

    ret = configure_i2s();
    if (ret < 0) {
        return ret;
    }
    return 0;
}
