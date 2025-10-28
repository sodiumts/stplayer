#include "audio_playback.h"
#include "opus_defines.h"
#include "syscalls/kernel.h"
#include "zephyr/kernel.h"

#include <string.h>
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
    const float MAX_AMPLITUDE = 0.6f; // clamp it to stop loudness
    
    if (adc_value == 0) {
        memset(samples, 0, nsamples * sizeof(int16_t));
        return;
    }

    if (adc_value > ADC_MAX) adc_value = ADC_MAX;

    float normalized = (float)adc_value / ADC_MAX;
    float scale = normalized * normalized;    

    if (scale > MAX_AMPLITUDE) {
        scale = MAX_AMPLITUDE;
    }

    for (size_t i = 0; i < nsamples; i++) {
        samples[i] = (int16_t)(samples[i] * scale);
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

void play_opus_packet(bool *isPlaying, int discard_cnt, uint16_t volume) {
    void * block = NULL;
    uint16_t packet_size;
    int rcf = opus_get_packet(&op_state, opus_packet, &packet_size, &filep);
    if (rcf != OP_OK && rcf != OP_DONE) {
        fs_close(&filep);
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        stop_i2s_dma();
        *isPlaying = false;
        return;
    }

    int rc = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
    if (rc < 0) {
        LOG_ERR("Block allocation failed: %d", rc);
        *isPlaying = false;
        return;
    }
    // Returns count of decoded samples
    int oprc = opus_decode(decoder, opus_packet, packet_size, block, SAMPLE_NO, 0);
    if (oprc < 0) {
        LOG_ERR("Opus decode failed: %d", oprc);
        fs_close(&filep);
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        stop_i2s_dma();
        *isPlaying = false;
        return;
    }

    if (discard_cnt > 0) {
        if (discard_cnt > SAMPLE_NO) {
            LOG_ERR("Discard count larger than one decoded buffer: %d vs %d", discard_cnt, SAMPLE_NO);
            fs_close(&filep);
            opus_decoder_ctl(decoder, OPUS_RESET_STATE);
            stop_i2s_dma();
            *isPlaying = false;
            return;
        }

        memmove((int16_t *) block, (int16_t *) block + discard_cnt, (oprc - discard_cnt)* sizeof(int16_t));
        oprc -= discard_cnt;
        discard_cnt = 0;
    }

    apply_volume_int16((int16_t *) block, oprc * 2, volume);

    rc = i2s_write(i2s_dev, block, BLOCK_SIZE);
    if (rc < 0) {
        LOG_ERR("i2s_write failed: %d", rc);
        k_mem_slab_free(&tx_0_mem_slab, &block);
        *isPlaying = false;
        return;
    }

    // Done with file
    if (rcf == OP_DONE) {
        LOG_INF("Done with file");
        fs_close(&filep);
        opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        stop_i2s_dma();
        *isPlaying = false;
        return;
    }
}

void audio_handler_thread(void *pipeP, void *arg2, void *arg3) {
    LOG_INF("Started audio");
    struct k_pipe* pipe = (struct k_pipe*) pipeP;
    int ret = init_audio_playback();
    if (ret < 0) {
        LOG_INF("Failed audio Init");
        return;
    }

    bool isPlaying = false;
    
    uint16_t discard_cnt = 0;
    uint16_t volume = 0;
    while(1) {
        audio_thread_msg receivedMessage;
        receivedMessage.msg_type = DEF;
        int ret = k_pipe_read(pipe, (uint8_t *) &receivedMessage, sizeof(audio_thread_msg), K_NSEC(1));
        if (ret < 0 && ret != -EAGAIN) {
            LOG_ERR("Read pipe error %d", ret);
        }

        switch(receivedMessage.msg_type) {
            case PLAY:
                // Set up a new opus file to be played
                int rc;
                isPlaying = true;
                fs_file_t_init(&filep);

                if ((rc = fs_open(&filep, receivedMessage.song_path, FS_O_READ)) < 0) {
                    LOG_ERR("fs_open failed: %d", rc); 
                    continue;
                }

                rc = start_i2s_dma();
                if (rc < 0) {
                    fs_close(&filep);
                    opus_decoder_ctl(decoder, OPUS_RESET_STATE);
                    stop_i2s_dma();
                    continue;
                }

                opus_state_init(&op_state);

                discard_cnt = opus_verify_header(&filep, &op_state); 
                if (discard_cnt < 0) {
                    fs_close(&filep);
                    stop_i2s_dma();
                }

                discard_cnt *= 2; 
            break;
            case VOL:
                volume = receivedMessage.volume; 
            break;
            case DEF:
            break;
            default:
                LOG_INF("Got message type %d", receivedMessage.msg_type);
            break;
        }

        if (isPlaying) {
            play_opus_packet(&isPlaying, discard_cnt, volume);
            // Discard count should be 0 after first packet
            if (discard_cnt > 0) discard_cnt = 0;
        }

    }
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
