#pragma once

#include <zephyr/fs/fs.h>
#include <ff.h>

#define STORAGE_PARTITION		    storage_partition
#define STORAGE_PARTITION_ID		FIXED_PARTITION_ID(STORAGE_PARTITION)

void setup_disk(void);
