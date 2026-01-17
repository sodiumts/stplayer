#include "audio_playback.h"
#include "opus_defines.h"
#include "syscalls/kernel.h"
#include "zephyr/kernel.h"

#include <math.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <opus.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>

LOG_MODULE_REGISTER(audio_playback, LOG_LEVEL_DBG);

static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

static uint8_t opus_packet[1275];
static opus_state_t op_state;
static struct fs_file_t filep;
OpusDecoder *decoder;

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS, 32);

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
    int oprc = opus_decode(decoder, opus_packet, packet_size, block, SAMPLE_NO, 0);

    if (discard_cnt > 0) {
        size_t discard_samples = discard_cnt * 2;

        if (discard_samples > (size_t)oprc) {
            LOG_ERR("Discard too large");
        }

        int16_t *audio_data = (int16_t *)block;
        memmove(audio_data, 
                audio_data + discard_samples,  // skip discards
                (oprc - discard_samples) * sizeof(int16_t));

        oprc -= discard_samples;
    }

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
static const struct device *codec;
void codec_initialize(void) {
    codec = DEVICE_DT_GET(DT_NODELABEL(codec0));
    if(!device_is_ready(codec)) {
        LOG_ERR("CODEC not ready");
        return;
    }

    struct audio_codec_cfg cfg = {0};
    cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
    cfg.dai_type = AUDIO_DAI_TYPE_I2S;
    cfg.mclk_freq = 12288000;

    cfg.dai_cfg.i2s.word_size = 16;
    cfg.dai_cfg.i2s.channels = CHANNELS;
    cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_DATA_ORDER_MSB;
    cfg.dai_cfg.i2s.options         = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE;
    cfg.dai_cfg.i2s.frame_clk_freq = SAMPLE_RATE;
    cfg.dai_cfg.i2s.block_size = BLOCK_SIZE;
    cfg.dai_cfg.i2s.mem_slab = &tx_0_mem_slab;
    
    int rc = audio_codec_configure(codec, &cfg);
    if (rc) {
        LOG_ERR("Audio configure failed: %d", rc);
        return;
    }    


    audio_codec_start_output(codec);
    
    audio_property_value_t v = {.vol = -40};
    rc = audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME, AUDIO_CHANNEL_ALL, v);
    if(rc) {
        LOG_ERR("Failed to set volume");
        return;
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

    codec_initialize();
    return 0;
}
