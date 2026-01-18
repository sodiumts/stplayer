#pragma once

#include <stdint.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <lvgl.h>

bool is_mounted(void);

void setup_disk(void);
int populate_list_with_files(lv_obj_t *list);

