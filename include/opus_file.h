#pragma once

#include <zephyr/kernel.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/fs_sys.h>
#include <zephyr/storage/flash_map.h>
#include <ff.h>

typedef struct {
    uint8_t segment_lengths[255];
    uint8_t page_segment_count;
    uint8_t segment_pos;
    uint8_t remaining_segments;

    _Bool is_eos_page;
} opus_state_t;

#define OGG_HEADER_SIZE 27
#define OPUS_HEAD_SIZE  19

#define OP_EOF   -1 // Eof for ogg file
#define OP_NOOGG -2 // Not ogg file
#define OP_MISS  -3 // Missmatching size
#define OP_NOOPUS -5 // File not opus
#define OP_NOTAGS -6 // Missing opus tags
#define OP_ZERO   -7 // zero length opus packet
#define OP_TOOLARGE -8 // Opus packet exceedes limit

#define OP_OK     1 // Success
#define OP_DONE   2 // Done reading ogg container

uint16_t opus_verify_header(struct fs_file_t* fp, opus_state_t *state);

void opus_state_init(opus_state_t *st);

int opus_get_packet(opus_state_t *st, uint8_t *buff, uint16_t *pack_size, struct fs_file_t *fp);
