#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/i2s.h>
#include <math.h>

#define SAMPLE_RATE     48000U
#define TONE_FREQ       440.0
#define BUFFER_FRAMES   256
#define CHANNELS        2
#define BYTES_PER_SAMPLE (sizeof(int16_t))
#define BUFFER_SIZE     (BUFFER_FRAMES * CHANNELS)

#ifndef M_PI
#define M_PI 3.1415926
#endif


#define I2S_DEV DT_NODELABEL(i2s2)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);

static int16_t audio_buf[BUFFER_SIZE];
static double phase;

static void fill_sine(void)
{
    const double phase_inc = 2.0 * M_PI * TONE_FREQ / (double)SAMPLE_RATE;

    for (int i = 0; i < BUFFER_FRAMES; i++) {
        int16_t sample = (int16_t)(sin(phase) * INT16_MAX);
        /* left + right same sample */
        audio_buf[2*i]   = sample;
        audio_buf[2*i+1] = sample;

        phase += phase_inc;
        if (phase >= 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    }
}

LOG_MODULE_REGISTER(main);

#include <zephyr/storage/flash_map.h>
#include <ff.h>

#define STORAGE_PARTITION		storage_partition
#define STORAGE_PARTITION_ID		FIXED_PARTITION_ID(STORAGE_PARTITION)
static struct fs_mount_t fs_mnt;

static int setup_flash(struct fs_mount_t *mnt)
{
	int rc = 0;
	unsigned int id;
	const struct flash_area *pfa;

	mnt->storage_dev = (void *)STORAGE_PARTITION_ID;
	id = STORAGE_PARTITION_ID;

	rc = flash_area_open(id, &pfa);
	printk("Area %u at 0x%x on %s for %u bytes\n",
	       id, (unsigned int)pfa->fa_off, pfa->fa_dev->name,
	       (unsigned int)pfa->fa_size);

	if (rc < 0 && IS_ENABLED(CONFIG_APP_WIPE_STORAGE)) {
		printk("Erasing flash area ... ");
		rc = flash_area_flatten(pfa, 0, pfa->fa_size);
		printk("%d\n", rc);
	}

	if (rc < 0) {
		flash_area_close(pfa);
	}
	return rc;
}

static int mount_app_fs(struct fs_mount_t *mnt)
{
	int rc;

	static FATFS fat_fs;

	mnt->type = FS_FATFS;
	mnt->fs_data = &fat_fs;
	if (IS_ENABLED(CONFIG_DISK_DRIVER_RAM)) {
		mnt->mnt_point = "/RAM:";
	} else if (IS_ENABLED(CONFIG_DISK_DRIVER_SDMMC)) {
		mnt->mnt_point = "/SD:";
	} else {
		mnt->mnt_point = "/NAND:";
	}

	rc = fs_mount(mnt);

	return rc;
}

static void setup_disk(void)
{
	struct fs_mount_t *mp = &fs_mnt;
	struct fs_dir_t dir;
	struct fs_statvfs sbuf;
	int rc;

	fs_dir_t_init(&dir);

	if (IS_ENABLED(CONFIG_DISK_DRIVER_FLASH)) {
		rc = setup_flash(mp);
		if (rc < 0) {
			LOG_ERR("Failed to setup flash area");
			return;
		}
	}

	if (!IS_ENABLED(CONFIG_FILE_SYSTEM_LITTLEFS) &&
	    !IS_ENABLED(CONFIG_FAT_FILESYSTEM_ELM)) {
		LOG_INF("No file system selected");
		return;
	}

	rc = mount_app_fs(mp);
	if (rc < 0) {
		LOG_ERR("Failed to mount filesystem");
		return;
	}

	/* Allow log messages to flush to avoid interleaved output */
	k_sleep(K_MSEC(50));

	printk("Mount %s: %d\n", fs_mnt.mnt_point, rc);

	rc = fs_statvfs(mp->mnt_point, &sbuf);
	if (rc < 0) {
		printk("FAIL: statvfs: %d\n", rc);
		return;
	}

	printk("%s: bsize = %lu ; frsize = %lu ;"
	       " blocks = %lu ; bfree = %lu\n",
	       mp->mnt_point,
	       sbuf.f_bsize, sbuf.f_frsize,
	       sbuf.f_blocks, sbuf.f_bfree);

	rc = fs_opendir(&dir, mp->mnt_point);
	printk("%s opendir: %d\n", mp->mnt_point, rc);

	if (rc < 0) {
		LOG_ERR("Failed to open directory");
	}

	while (rc >= 0) {
		struct fs_dirent ent = { 0 };

		rc = fs_readdir(&dir, &ent);
		if (rc < 0) {
			LOG_ERR("Failed to read directory entries");
			break;
		}
		if (ent.name[0] == 0) {
			printk("End of files\n");
			break;
		}
		printk("  %c %u %s\n",
		       (ent.type == FS_DIR_ENTRY_FILE) ? 'F' : 'D',
		       ent.size,
		       ent.name);
	}

	(void)fs_closedir(&dir);

	return;
}


int main(void)
{
	int ret;

	setup_disk();

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return 0;
	}

	LOG_INF("The device is put in USB mass storage mode.\n");

    if (!device_is_ready(i2s_dev)) {
        LOG_ERR("I2S device not ready");
        return 1;
    }
    

    struct i2s_config cfg = {
        .word_size       = 16,
        .channels        = CHANNELS,
        .format          = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_DATA_ORDER_MSB,
        .options         = I2S_OPT_BIT_CLK_MASTER | I2S_OPT_FRAME_CLK_MASTER,
        .frame_clk_freq  = SAMPLE_RATE,
        .block_size      = BUFFER_SIZE * BYTES_PER_SAMPLE,
        .timeout         = 100
    };

    ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
    if (ret) {
        printk("I2S config failed: %d\n", ret);
        return 1;
    }

    k_sleep(K_MSEC(100)); 

    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret) {
        printk("I2S trigger start failed: %d\n", ret);
        return 1;
    }

    while (1) {
        fill_sine();

        ret = i2s_write(i2s_dev, audio_buf, sizeof(audio_buf));
        if (ret) {
            printk("I2S write error: %d\n", ret);
            break;
        }
    }

    i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_STOP);

	return 0;
}
