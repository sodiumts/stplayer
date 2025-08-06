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




#define I2S_DEV DT_NODELABEL(i2s3)
static const struct device *i2s_dev = DEVICE_DT_GET(I2S_DEV);



LOG_MODULE_REGISTER(main);


#define SAMPLE_NO 64

/* The data represent a sine wave */
static int16_t data[SAMPLE_NO] = {
	  3211,   6392,   9511,  12539,  15446,  18204,  20787,  23169,
	 25329,  27244,  28897,  30272,  31356,  32137,  32609,  32767,
	 32609,  32137,  31356,  30272,  28897,  27244,  25329,  23169,
	 20787,  18204,  15446,  12539,   9511,   6392,   3211,      0,
	 -3212,  -6393,  -9512, -12540, -15447, -18205, -20788, -23170,
	-25330, -27245, -28898, -30273, -31357, -32138, -32610, -32767,
	-32610, -32138, -31357, -30273, -28898, -27245, -25330, -23170,
	-20788, -18205, -15447, -12540,  -9512,  -6393,  -3212,     -1,
};

static void fill_buf(int16_t *tx_block, int att)
{
	int r_idx;

	for (int i = 0; i < SAMPLE_NO; i++) {
		/* Left channel is sine wave */
		tx_block[2 * i] = data[i] / (1 << att);
		/* Right channel is same sine wave, shifted by 90 degrees */
		r_idx = (i + (ARRAY_SIZE(data) / 4)) % ARRAY_SIZE(data);
		tx_block[2 * i + 1] = data[r_idx] / (1 << att);
	}
}


#define NUM_BLOCKS 20
#define BLOCK_SIZE (2 * sizeof(data))


static char __aligned(WB_UP(32))
                      _k_mem_slab_buf_tx_0_mem_slab[(NUM_BLOCKS) * WB_UP(BLOCK_SIZE)];
static STRUCT_SECTION_ITERABLE(k_mem_slab, tx_0_mem_slab) = Z_MEM_SLAB_INITIALIZER(tx_0_mem_slab, _k_mem_slab_buf_tx_0_mem_slab, WB_UP(BLOCK_SIZE), NUM_BLOCKS);

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

static uint32_t phase = 0;  // Phase accumulator for continuous wave

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

	//setup_disk();

	//ret = usb_enable(NULL);
	//if (ret != 0) {
	//	LOG_ERR("Failed to enable USB\n");
	//	return 0;
	//}

	LOG_INF("The device is put in USB mass storage mode.\n");

    if (!device_is_ready(i2s_dev)) {
        LOG_ERR("I2S device not ready\n");
        return 1;
    }
    
    struct i2s_config cfg = {
        .word_size       = 16,
        .channels        = 2,
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
