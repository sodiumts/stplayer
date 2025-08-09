#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(oggparse)

#define MAX_OPUS_PACKET_SIZE 2000
#define OGG_PAGE_HEADER_SIZE 27

struct ogg_parser {
    uint8_t state;
    uint8_t header[OGG_PAGE_HEADER_SIZE];
    uint8_t seg_table[255];
    size_t header_bytes;
    size_t seg_table_bytes;
    uint8_t nsegs;
    uint8_t current_seg;
    uint16_t current_seg_remaining;
    uint8_t packet_buf[MAX_OPUS_PACKET_SIZE];
    size_t packet_size;
    bool headers_done;
    uint32_t packet_count;
};

enum parser_state {
    STATE_HEADER,
    STATE_SEGMENT_TABLE,
    STATE_SEGMENTS
};


void ogg_parser_init(struct ogg_parser *p) {
    memset(p, 0, sizeof(*p));
    p->state = STATE_HEADER;
}


