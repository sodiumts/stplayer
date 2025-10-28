#pragma once

#include "opus_file.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/adc.h>
#include <opus.h>

#define SAMPLE_RATE 48000
#define SAMPLE_NO 2880
#define NUM_BLOCKS 2
#define CHANNELS 2
#define BLOCK_SIZE (SAMPLE_NO * CHANNELS * sizeof(int16_t))

#define I2S_DEV DT_NODELABEL(i2s3)

int stream_opus(const char *path, const struct adc_dt_spec *adc_chan);
int init_audio_playback();

void audio_handler_thread(void *pipeP, void *arg2, void *arg3);

enum message_type {PLAY, PAUSE, RESUME, VOL, DEF};

typedef struct {
    enum message_type msg_type;
    int volume;
    char song_path[64];
} audio_thread_msg;
