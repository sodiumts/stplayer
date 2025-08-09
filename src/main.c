#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/usb/usb_device.h>
#include <ff.h>
#include <opus.h>

#include "usb_mass.h"
#include "opus_file.h"

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

#define I2S_DEV DT_NODELABEL(i2s3)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

LOG_MODULE_REGISTER(main);

OpusDecoder *decoder;

#define SAMPLE_NO 960
#define NUM_BLOCKS 2
#define CHANNELS 2
#define BLOCK_SIZE (SAMPLE_NO * CHANNELS * sizeof(int16_t))

static uint8_t opus_packet[500];
static opus_state_t op_state;
static struct fs_file_t filep;

K_MEM_SLAB_DEFINE_STATIC(tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS, 32);

int stream_wav(const char *path) {
    int rc;
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
    
    uint16_t packet_size;
    uint32_t packet_cnt = 0;
    while(1) {
        void * block = NULL;
        //LOG_INF("Getting packet");
        rc = opus_get_packet(&op_state, opus_packet, &packet_size, &filep);
        // Failed
        if (rc != OP_OK && rc != OP_DONE)
            goto cleanup;

        //LOG_INF("Allocating block");

        packet_cnt++;
        rc = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
        if (rc < 0) {
            LOG_ERR("Block allocation failed: %d", rc);
            break;
        }

        //LOG_INF("Decoding...");
        int oprc = opus_decode(decoder, opus_packet, packet_size, block, SAMPLE_NO, 0);
        if (oprc < 0) {
            LOG_ERR("Opus decode failed: %d", oprc);
            goto cleanup;
        }
        
        //LOG_INF("Decoded count: %d", oprc);
        //apply_volume_int16((int16_t *) block, oprc, 5);
        
        //k_sleep(K_MSEC(50));
        rc = i2s_write(i2s_dev, block, BLOCK_SIZE);
        if (rc < 0) {
            LOG_ERR("i2s_write failed: %d", rc);
            k_mem_slab_free(&tx_0_mem_slab, &block);
            break;
        }



        // Done with file
        if (rc == OP_DONE) {
            LOG_INF("Done with file");
            goto cleanup;
        }
    }
  //  uint8_t riff_header[12];
  //  uint32_t data_size = 0;
  //  uint32_t sample_rate = 0;
  //  uint16_t bits_per_sample = 0;
  //  uint16_t num_channels = 0;

  //  if (fs_read(&filep, riff_header, sizeof(riff_header)) != sizeof(riff_header)) {
  //      LOG_ERR("Failed to read RIFF header");
  //      rc = -EIO;
  //      goto cleanup;
  //  }

  //  if (memcmp(riff_header, "RIFF", 4) != 0 || memcmp(riff_header + 8, "WAVE", 4) != 0) {
  //      LOG_ERR("Not a valid WAV file");
  //      rc = -EINVAL;
  //      goto cleanup;
  //  }

  //  while (1) {
  //      char chunk_id[4];
  //      uint32_t chunk_size;
  //      
  //      if (fs_read(&filep, chunk_id, 4) != 4) {
  //          LOG_ERR("Failed to read chunk ID");
  //          rc = -EIO;
  //          goto cleanup;
  //      }

  //      if (fs_read(&filep, &chunk_size, 4) != 4) {
  //          LOG_ERR("Failed to read chunk size");
  //          rc = -EIO;
  //          goto cleanup;
  //      }

  //      if (memcmp(chunk_id, "fmt ", 4) == 0) {
  //          struct __attribute__((packed)) {
  //              uint16_t audio_format;
  //              uint16_t num_channels;
  //              uint32_t sample_rate;
  //              uint32_t byte_rate;
  //              uint16_t block_align;
  //              uint16_t bits_per_sample;
  //          } fmt_chunk;

  //          if (fs_read(&filep, &fmt_chunk, sizeof(fmt_chunk)) != sizeof(fmt_chunk)) {
  //              LOG_ERR("Failed to read fmt chunk");
  //              rc = -EIO;
  //              goto cleanup;
  //          }

  //          if (chunk_size > sizeof(fmt_chunk)) {
  //              fs_seek(&filep, chunk_size - sizeof(fmt_chunk), FS_SEEK_CUR);
  //          }

  //          if (fmt_chunk.audio_format != 1) {  // 1 = PCM
  //              LOG_ERR("Only PCM format supported");
  //              rc = -ENOTSUP;
  //              goto cleanup;
  //          }
  //          
  //          num_channels = fmt_chunk.num_channels;
  //          sample_rate = fmt_chunk.sample_rate;
  //          bits_per_sample = fmt_chunk.bits_per_sample;
  //      }
  //      else if (memcmp(chunk_id, "data", 4) == 0) {
  //          data_size = chunk_size;
  //          break;
  //      }
  //      else {
  //          fs_seek(&filep, chunk_size, FS_SEEK_CUR);
  //      }
  //  }

  //  if (num_channels != CHANNELS) {
  //      LOG_ERR("Unsupported number of channels: %d (expected %d)", 
  //              num_channels, CHANNELS);
  //      rc = -ENOTSUP;
  //      goto cleanup;
  //  }

  //  if (bits_per_sample != 16) {
  //      LOG_ERR("Only 16-bit samples supported");
  //      rc = -ENOTSUP;
  //      goto cleanup;
  //  }

  //  if (sample_rate != 48000) {
  //      LOG_WRN("Sample rate mismatch: file=%dHz, configured=48000Hz", sample_rate);
  //  }

  //  LOG_INF("Channels: %d bits per sample: %d sample rate %d", num_channels, bits_per_sample, sample_rate);

  //  size_t bytes_remaining = data_size;
  //  while (bytes_remaining > 0) {
  //      void *block = NULL;
  //      ssize_t read_n = 0;

  //      rc = k_mem_slab_alloc(&tx_0_mem_slab, &block, K_FOREVER);
  //      if (rc < 0) {
  //          LOG_ERR("Block allocation failed: %d", rc);
  //          break;
  //      }

  //      size_t to_read = MIN(BLOCK_SIZE, bytes_remaining);
  //      read_n = fs_read(&filep, block, to_read);
  //      if (read_n < 0) {
  //          LOG_ERR("Read failed: %d", (int)read_n);
  //          k_mem_slab_free(&tx_0_mem_slab, &block);
  //          rc = (int)read_n;
  //          break;
  //      }

  //      if ((size_t)read_n < BLOCK_SIZE) {
  //          memset((uint8_t *)block + read_n, 0, BLOCK_SIZE - (size_t)read_n);
  //      }

  //      size_t nsamples = BLOCK_SIZE / sizeof(int16_t);
  //      apply_volume_int16((int16_t *) block, nsamples, 5);

  //      rc = i2s_write(i2s_dev, block, BLOCK_SIZE);
  //      if (rc < 0) {
  //          LOG_ERR("i2s_write failed: %d", rc);
  //          k_mem_slab_free(&tx_0_mem_slab, &block);
  //          break;
  //      }

  //      /* success: subtract bytes read */
  //      bytes_remaining -= read_n;
  //  }

  //  LOG_INF("Streamed %u bytes of PCM data", data_size - bytes_remaining);
cleanup:
    fs_close(&filep);
    return rc;
}

int main(void)
{
	int ret;

	
    //ret = usb_enable(NULL);
	//if (ret < 0) {
	//	LOG_ERR("Failed to enable USB\n");
    //    return 1;
	//}
	setup_disk();

	LOG_INF("Creating opus decoder.\n");


    
    decoder = opus_decoder_create(48000, CHANNELS, &ret);
    if (ret != OPUS_OK) {
        LOG_ERR("Failed to create decoder: %d", ret);
        return ret;
    }

    int size = opus_decoder_get_size(CHANNELS);
    LOG_INF("Opus decoder size: %d", size);

    if (!device_is_ready(i2s_dev)) {
        LOG_ERR("I2S device not ready\n");
        return 1;
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
    printk("Before config\n");
    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret) {
        printk("I2S config failed: %d\n", ret);
        return 1;
    }
    
    for (int i = 0; i < 2; i++){
        void *init_block;
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &init_block, K_FOREVER);
        if (ret < 0) {
            LOG_ERR("TX block alloc failed: %d\n", ret);
            return ret;
        }
        /* Zero-fill initial buffers (silence) to avoid pops.
           Alternatively fill with silence/fade-in or an initial decoded chunk. */
        memset(init_block, 0, BLOCK_SIZE);

        ret = i2s_write(i2s_dev, init_block, BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("i2s_write initial block failed: %d\n", ret);
            /* If driver doesn't accept block, free it */
            k_mem_slab_free(&tx_0_mem_slab, &init_block);
        }
    }

    /* Start I2S DMA consuming queued blocks */
    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret) {
        LOG_ERR("I2S trigger start failed: %d\n", ret);
        return 1;
    }

    LOG_INF("After trigger");
    const char *song_path = "/NAND:/BRUTO~1.OPU"; /* update filename to your file */

    int rc = stream_wav(song_path);
    printk("STREAM: stream_wav returned %d\n", rc);
    if (rc != 0) {
        LOG_ERR("stream_wav returned %d", rc);
    }
    /* optionally trigger drain/stop here */
    //i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);

    return 0;
}
