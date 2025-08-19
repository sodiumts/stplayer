#include "usb_mass.h"

#include "zephyr/kernel.h"
#include "zephyr/logging/log.h"
#include "zephyr/usb/usbd.h"
#include "zephyr/usb/usbd_msg.h"
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/storage/flash_map.h>
#include <ff.h>

#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/bos.h>


static const char *const blocklist[] = {
"dfu_dfu",
	NULL,
};

LOG_MODULE_REGISTER(usb_mass, LOG_LEVEL_DBG);

USBD_DEVICE_DEFINE(usbdev, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), 0x001, 0x001);

USBD_DESC_LANG_DEFINE(langthing);
USBD_DESC_MANUFACTURER_DEFINE(thingman, "Thingo manuf");
USBD_DESC_PRODUCT_DEFINE(thingprod, "Opus player");

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Config");
USBD_DESC_CONFIG_DEFINE(hs_cfg_desc, "HS Config");

static const uint8_t attributes = USB_SCD_REMOTE_WAKEUP;

USBD_CONFIGURATION_DEFINE(fs_config, attributes, 125, &fs_cfg_desc);
USBD_CONFIGURATION_DEFINE(hs_config, attributes, 125, &hs_cfg_desc);

USBD_DEFINE_MSC_LUN(sd, "SD", "Zephyr", "SD", "0.00");

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


static void fix_code_triple(struct usbd_context *uds_ctx,
				   const enum usbd_speed speed)
{
	/* Always use class code information from Interface Descriptors */
	if (IS_ENABLED(CONFIG_USBD_CDC_ACM_CLASS) ||
	    IS_ENABLED(CONFIG_USBD_CDC_ECM_CLASS) ||
	    IS_ENABLED(CONFIG_USBD_CDC_NCM_CLASS) ||
	    IS_ENABLED(CONFIG_USBD_MIDI2_CLASS) ||
	    IS_ENABLED(CONFIG_USBD_AUDIO2_CLASS) ||
	    IS_ENABLED(CONFIG_USBD_VIDEO_CLASS)) {
		/*
		 * Class with multiple interfaces have an Interface
		 * Association Descriptor available, use an appropriate triple
		 * to indicate it.
		 */
		usbd_device_set_code_triple(uds_ctx, speed,
					    USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	} else {
		usbd_device_set_code_triple(uds_ctx, speed, 0, 0, 0);
	}
}

static void msg_cb(struct usbd_context *const usbd_ctx, const struct usbd_msg *const msg) {
    LOG_INF("USBD message: %s", usbd_msg_type_string(msg->type));
    if (msg->type == USBD_MSG_CONFIGURATION) {
        k_work_submit(&unmount_work);
	}

    if (msg->type == USBD_MSG_SUSPEND) {
        k_work_submit(&mount_work);
    } 
}

int setup_mass(void) {
    int err;

    err = usbd_add_descriptor(&usbdev, &langthing);
	if (err) {
		LOG_ERR("Failed to initialize language descriptor (%d)", err);
		return err;
	}

    err = usbd_add_descriptor(&usbdev, &thingman);
	if (err) {
		LOG_ERR("Failed to initialize manufacturer descriptor (%d)", err);
		return err;
	}

    err = usbd_add_descriptor(&usbdev, &thingprod);
	if (err) {
		LOG_ERR("Failed to initialize product descriptor (%d)", err);
		return err;
	}

    if(USBD_SUPPORTS_HIGH_SPEED && usbd_caps_speed(&usbdev) == USBD_SPEED_HS) {
        LOG_INF("Supports HS");
        err = usbd_add_configuration(&usbdev, USBD_SPEED_HS, &hs_config);
        if (err) {
			LOG_ERR("Failed to add High-Speed configuration");
			return err;
		}
        err = usbd_register_all_classes(&usbdev, USBD_SPEED_HS, 1, blocklist);
        if (err) {
			LOG_ERR("Failed to add register classes");
			return err;
		}
        fix_code_triple(&usbdev, USBD_SPEED_HS);
    }

    err = usbd_add_configuration(&usbdev, USBD_SPEED_FS, &fs_config);
	if (err) {
		LOG_ERR("Failed to add Full-Speed configuration");
		return err;
	}

    err = usbd_register_all_classes(&usbdev, USBD_SPEED_FS, 1, blocklist);
	if (err) {
		LOG_ERR("Failed to add register classes");
		return err;
	}

    fix_code_triple(&usbdev, USBD_SPEED_FS);

    usbd_self_powered(&usbdev, attributes & USB_SCD_SELF_POWERED);

    
    err = usbd_init(&usbdev);
    if (err) {
		LOG_ERR("Failed to initialize device support");
		return err;
	}

    err = usbd_enable(&usbdev);
    if (err) {
        LOG_ERR("Failed to enable device support");
        return err;
    }


    err = usbd_msg_register_cb(&usbdev, msg_cb);
    if (err != 0) {
        LOG_ERR("Failed to register CB");
        return err;
    }

    LOG_INF("Device put into USB MASS storage mode");

    return 0;
}


