#include "usb_mass.h"

#include "core/lv_obj.h"
#include "misc/lv_color.h"
#include "zephyr/kernel.h"
#include "zephyr/logging/log.h"
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/storage/flash_map.h>
#include <ff.h>

#include <zephyr/device.h>



LOG_MODULE_REGISTER(usb_mass, LOG_LEVEL_DBG);

static struct fs_mount_t fs_mnt;
static bool fs_mounted = false;

static void mount_handler(struct k_work *work);
static void unmount_handler(struct k_work *work);

K_WORK_DEFINE(mount_work, mount_handler);
K_WORK_DEFINE(unmount_work, unmount_handler);

bool is_mounted(void) {
    return fs_mounted;
}

static void mount_handler(struct k_work *work) {
    if(!fs_mounted) {
        LOG_INF("Mounting due to MSC suspend");
        int res = fs_mount(&fs_mnt);
        if (res != 0) {
            LOG_ERR("Failed to mount FS: %d", res);
            return;
        }
        fs_mounted = true;
    }
}

static void unmount_handler(struct k_work *work) {
    if(fs_mounted) {
        LOG_INF("Unmounting due to MSC connect");
        int res = fs_unmount(&fs_mnt);
        if(res != 0) {
            LOG_INF("Failed to mount fs: %d", res);
            return;
        }
        fs_mounted = false;
    }
}

static int mount_app_fs(struct fs_mount_t *mnt)
{
	int rc;

	static FATFS fat_fs;

	mnt->type = FS_FATFS;
	mnt->fs_data = &fat_fs;
	mnt->mnt_point = "/SD:";

	rc = fs_mount(mnt);
    fs_mounted = true;
    printk("fs_mount returned %d for mount point %s\n", rc, mnt->mnt_point);

	return rc;
}

void setup_disk(void)
{
	struct fs_mount_t *mp = &fs_mnt;
	struct fs_dir_t dir;
	struct fs_statvfs sbuf;
	int rc;

    
	fs_dir_t_init(&dir);


    rc = disk_access_ioctl("SD", DISK_IOCTL_CTRL_INIT, NULL);
    if (rc != 0) {
        LOG_ERR("Failed to init SD: %d", rc);
        return;
    }

	rc = mount_app_fs(mp);
    printk("mount_app_fs rc = %d\n", rc);
	if (rc < 0) {
		LOG_ERR("Failed to mount filesystem");
		return;
	}

	/* Allow log messages to flush to avoid interleaved output */

	printk("Mount %s: %d\n", fs_mnt.mnt_point, rc);

	
//    rc = fs_statvfs(mp->mnt_point, &sbuf);
//    printk("fs_statvfs rc = %d\n", rc);
//	if (rc < 0) {
//		printk("FAIL: statvfs: %d\n", rc);
//		return;
//	}
//
//	printk("%s: bsize = %lu ; frsize = %lu ;"
//	       " blocks = %lu ; bfree = %lu\n",
//	       mp->mnt_point,
//	       sbuf.f_bsize, sbuf.f_frsize,
//	       sbuf.f_blocks, sbuf.f_bfree);

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

int populate_list_with_files(lv_obj_t *list) {
    struct fs_dir_t dir;
    struct fs_mount_t *mp = &fs_mnt;
    int rc;

    fs_dir_t_init(&dir);

    rc = fs_opendir(&dir, mp->mnt_point);
    if (rc < 0) {
        LOG_ERR("Failed to open directory: %d", rc);
        return rc;
    }

    // Clear existing list items if needed
    // lv_obj_clean(list); // Uncomment if you want to clear first

    int file_count = 0;
    
    while (1) {
        struct fs_dirent ent = {0};
        
        rc = fs_readdir(&dir, &ent);
        if (rc < 0) {
            LOG_ERR("Failed to read directory entry");
            break;
        }
        
        if (ent.name[0] == 0) {
            break;
        }

        if (ent.type == FS_DIR_ENTRY_FILE) {

            lv_obj_t *list_item = lv_list_add_text(list, ent.name);
            lv_obj_set_style_bg_color(list_item, lv_color_black(), LV_PART_MAIN); 
            lv_obj_set_style_text_color(list_item, lv_color_white(), 0);
            
            file_count++;
            
            printk("Added file to list: %s\n", ent.name);
        }
    }

    fs_closedir(&dir);
    LOG_INF("Added %d files to list", file_count);
    return file_count;
}
