#include "audio_playback.h"
#include "opus_defines.h"

#include <zephyr/logging/log.h>
#include <opus.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>

LOG_MODULE_REGISTER(audio_playback, LOG_LEVEL_DBG);

static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

static uint8_t opus_packet[1275];
static opus_state_t op_state;
static struct fs_file_t filep;
OpusDecoder *decoder;

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS, 32);

static void apply_volume_int16(int16_t *samples, size_t nsamples, uint16_t adc_value)
{
    const uint16_t ADC_MAX = 4000;
    const float MAX_AMPLITUDE = 0.6f; // Never exceed 60% to prevent clipping
    
    if (adc_value == 0) {
        memset(samples, 0, nsamples * sizeof(int16_t));
        return;
    }

    if (adc_value > ADC_MAX) adc_value = ADC_MAX;

    // Exponential curve for better low-end control
    float normalized = (float)adc_value / ADC_MAX;
    float scale = normalized * normalized; // x^2 curve (softer than x^3)
    
    // Apply hard limit
    if (scale > MAX_AMPLITUDE) {
        scale = MAX_AMPLITUDE;
    }

    for (size_t i = 0; i < nsamples; i++) {
        samples[i] = (int16_t)(samples[i] * scale);
    }
}


uint16_t read_potentiometer(const struct adc_dt_spec *adc_chan)
{
    int ret;
    uint16_t buf;
    struct adc_sequence sequence = {
        .buffer = &buf,
        .buffer_size = sizeof(buf),
        .oversampling = 4
    };

    if (!device_is_ready(adc_chan->dev)) {
        printk("ADC device not ready\n");
        return -1;
    }

    ret = adc_sequence_init_dt(adc_chan, &sequence);
    if (ret != 0) {
        printk("Failed to initialize ADC sequence: %d\n", ret);
        return ret;
    }

    ret = adc_read(adc_chan->dev, &sequence);
    if (ret != 0) {
        printk("Failed to read ADC: %d\n", ret);
        return ret;
    }

    // Scale to 0-100 (12-bit ADC: max = 4095)
    //int percentage = (buf * 100) / ((1 << adc_chan.resolution) - 1);

    //printk("PA2 - ADC raw: %d, Percentage: %d%%\n", buf, percentage);

    return buf;
}

//#define NUM_SAMPLES 16  // Number of samples for averaging
//
//uint16_t read_potentiometer_avg(const struct adc_dt_spec *adc_chan) {
//    uint32_t sum = 0;
//    for (int i = 0; i < NUM_SAMPLES; i++) {
//        sum += read_potentiometer(adc_chan); // Your existing function
//    }
//    return (uint16_t)(sum / NUM_SAMPLES);
//}

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

    LOG_INF("Starting i2s DMAs");
    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret) {
        LOG_ERR("I2S trigger start failed: %d", ret);
        return -1;
    }
    return 0;
}

static int stop_i2s_dma() {
    LOG_INF("Stopping i2s DMAs");
    int ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    if (ret < 0) {
        LOG_ERR("Failed to stop i2s with drain: %d", ret);
        return ret;
    }
    return 0;
}

int stream_opus(const char *path, const struct adc_dt_spec *adc_chan) {
    int rc;
    fs_file_t_init(&filep);

    if ((rc = fs_open(&filep, path, FS_O_READ)) < 0) {
        LOG_ERR("fs_open failed: %d", rc); 
        return rc;
    }

    rc = start_i2s_dma();
    if (rc < 0) {
        goto cleanup;
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

        //LOG_INF("Decode count: %d", oprc);

        if (discard_cnt > 0) {
            if (discard_cnt > SAMPLE_NO) {
                LOG_ERR("Discard count larger than one decoded buffer: %d vs %d", discard_cnt, SAMPLE_NO);
                goto cleanup;
            }

            memmove((int16_t *) block, (int16_t *) block + discard_cnt, (oprc - discard_cnt)* sizeof(int16_t));
            oprc -= discard_cnt;
            discard_cnt = 0;
        }

        apply_volume_int16((int16_t *) block, oprc * 2, read_potentiometer(adc_chan));

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
    opus_decoder_ctl(decoder, OPUS_RESET_STATE);
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
        .frame_clk_freq  = SAMPLE_RATE,
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
    decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &ret);
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
