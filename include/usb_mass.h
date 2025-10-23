#pragma once

#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <ff.h>

//#define STORAGE_PARTITION		    storage_partition
//#define STORAGE_PARTITION_ID		FIXED_PARTITION_ID(STORAGE_PARTITION)

bool is_mounted(void);

void setup_disk(void);

