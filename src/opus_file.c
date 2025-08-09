#include "opus_file.h"
#include "zephyr/kernel.h"
#include "zephyr/logging/log_core.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(opus_file, LOG_LEVEL_DBG);

static uint8_t ogg_header[OGG_HEADER_SIZE];
uint16_t opus_verify_header(struct fs_file_t* fp, opus_state_t *state) {
    size_t rd = fs_read(fp, ogg_header, OGG_HEADER_SIZE);
    if (rd != OGG_HEADER_SIZE) {
        LOG_ERR("OGG header size read did not match: got %d : expected %d", (int) rd, OGG_HEADER_SIZE);
        return OP_MISS;
    }
    
    if (memcmp(ogg_header, "OggS", 4) != 0) {
        LOG_ERR("File is not valid OGG");
        return OP_NOOGG;
    }

    
    
    uint8_t segment_num = ogg_header[26];
    //printf("Number of segments: %d\n", segment_num);
 
    fs_seek(fp, segment_num, FS_SEEK_CUR);

    static uint8_t opus_head[OPUS_HEAD_SIZE];

    rd = fs_read(fp, opus_head, OPUS_HEAD_SIZE);
    if (rd != OPUS_HEAD_SIZE) {
        LOG_ERR("OPUSHEAD size read did not match: got %d : expected %d", (int) rd, OGG_HEADER_SIZE);
        return OP_MISS;
    }



    static const char opus_magic [] = "OpusHead";
    if (memcmp(opus_head, opus_magic, 8) != 0) {
        LOG_ERR("File is not an opus ogg");
        return OP_NOOPUS;
    }

    LOG_DBG("Stream version: %d", opus_head[8]);

    LOG_DBG("Channel count: %d", opus_head[9]);
    uint16_t discard_samples = ((uint16_t) opus_head[10]) | ((uint16_t)opus_head[11] << 8);
    LOG_DBG("Discard samples: %d", discard_samples);
    uint32_t sample_rate = ((uint32_t) opus_head[12]) | ((uint32_t) opus_head[13] << 8) | ((uint32_t) opus_head[14] << 16) | ((uint32_t) opus_head[15] << 24);
    LOG_DBG("Sample rate: %d", sample_rate);
    uint16_t output_gain = ((uint16_t) opus_head[16]) | ((uint16_t) opus_head[17] << 8);
    LOG_DBG("Output gain: %d", output_gain);
    
    rd = fs_read(fp, ogg_header, OGG_HEADER_SIZE);
    if (rd != OGG_HEADER_SIZE) {
        LOG_ERR("OGG header size read did not match: got %d : expected %d", (int) rd, OGG_HEADER_SIZE);
        return OP_MISS;
    }
    
    if (memcmp(ogg_header, "OggS", 4) != 0) {
        LOG_ERR("Second page is not valid OGG");
        return OP_NOOGG;
    }

    segment_num = ogg_header[26];
    //printf("Second page segment count: %d\n", segment_num);

    char segment_table[segment_num];
    rd = fs_read(fp, segment_table, segment_num);
    if (rd != segment_num) {
        LOG_ERR("Did not read enough segment lengths");
        return OP_MISS;
    }
    
    uint16_t opus_tags_size = 0;
    for (uint8_t i = 0; i < segment_num; i++) {
        //printf("Segment (%d) length: %"PRIu8"\n", i, (uint8_t) segment_table[i]);
        opus_tags_size += (uint8_t) segment_table[i];
    }

    //printf("OpusTags size: %"PRIu16"\n", opus_tags_size);
    
    char opus_tags[opus_tags_size];
    rd = fs_read(fp, opus_tags, (uint16_t) opus_tags_size);
    if (rd != opus_tags_size) {
        LOG_ERR("Did not read enough segment lengths for opusTags");
        return OP_MISS;
    }


    if(memcmp("OpusTags", opus_tags, 8) != 0) {
        LOG_ERR("OpusTags does not exist");
        return OP_NOTAGS;
    }
    
    uint32_t vendor_len = ((uint32_t) opus_tags[8]) | ((uint32_t) opus_tags[9] << 8) | ((uint32_t) opus_tags[10] << 16) | ((uint32_t) opus_tags[11] << 24);
    LOG_DBG("%.*s", vendor_len, opus_tags + 12);

    uint32_t new_ptr = 12 + vendor_len;

    uint32_t user_comment_count =((uint32_t) opus_tags[new_ptr]) | ((uint32_t) opus_tags[new_ptr + 1] << 8) | ((uint32_t) opus_tags[new_ptr + 2] << 16) | ((uint32_t) opus_tags[new_ptr + 3] << 24); 
    LOG_DBG("User comment count: %"PRIu32"", (uint32_t) user_comment_count);

    new_ptr += 4;
    
    LOG_DBG("User tags: ");
    for (uint32_t i = 0; i < user_comment_count; i++) {
        uint32_t comment_len = ((uint32_t) opus_tags[new_ptr]) | ((uint32_t) opus_tags[new_ptr + 1] << 8) | ((uint32_t) opus_tags[new_ptr + 2] << 16) | ((uint32_t) opus_tags[new_ptr + 3] << 24);
        new_ptr += 4;
        LOG_DBG("%.*s", comment_len, opus_tags + new_ptr);
        new_ptr += comment_len;
    }

    //page_count += 2;
    return discard_samples;
}

void opus_state_init(opus_state_t *st) {
    memset(st, 0, sizeof(opus_state_t));
}

static int _opus_open_page(opus_state_t *st, struct fs_file_t *fp) {
    size_t rd;
    rd = fs_read(fp, ogg_header, OGG_HEADER_SIZE);
    if(rd != OGG_HEADER_SIZE) {
       // if(feof(fp)) {
       //     LOG_DBG("Reached end of file");
       //     return OP_EOF;
       // }
        LOG_ERR("Failed to read OGG header");
        return OP_MISS;
    }
    //page_count += 1;

    if (memcmp("OggS", ogg_header, 4) != 0) {
        LOG_ERR("Next segment is not OggS stream");
        return OP_NOOGG;
    }
    uint8_t segment_count = ogg_header[26];

    uint8_t header_type = ogg_header[5];
    st->is_eos_page = (header_type & 0x04) != 0;
    if (st->is_eos_page) {
        LOG_DBG("Reached last stream page");
    }

    //printf("New page segment count %"PRIu8"\n", segment_count);
    st->remaining_segments = segment_count;
    st->page_segment_count = segment_count;
    st->segment_pos = 0;

    rd = fs_read(fp, st->segment_lengths, segment_count);
    if (rd != segment_count) {
        LOG_ERR("Read segment count does not match real segment count");
        return OP_MISS;
    }

    return OP_OK;
}

int opus_get_packet(opus_state_t *st, uint8_t *buff, uint16_t *pack_size, struct fs_file_t *fp) {

    size_t rd;
    int res;
    
    // If we should start reading the next page
    if (st->remaining_segments == 0) {
        res = _opus_open_page(st, fp); 
        if (res < 0) return res;
    }
    
    uint16_t opus_len = 0;
    while(1) {
        if (st->remaining_segments == 0) {
            LOG_DBG("Page wrap occured");
            res = _opus_open_page(st, fp);
            if (res < 0) return res;
        }

        uint8_t seg = st->segment_lengths[st->segment_pos];
        opus_len += seg;

        st->remaining_segments -= 1;
        st->segment_pos += 1;

        if(st->segment_pos >= st->page_segment_count) {
            st->segment_pos = st->page_segment_count;
        }

        if (seg < 255) break;
    } 

    if (opus_len == 0) {
        LOG_ERR("Opus packet length 0");
        return OP_ZERO;
    }

    if(opus_len > 0xFFFF) {
        LOG_ERR("Opus packet exceeded RFC limit");
        return OP_TOOLARGE;
    }

    //printf("Opus packet len: %"PRIu16"\n", opus_len);
    *pack_size = opus_len;
    
    rd = fs_read(fp, buff, opus_len);
    if (rd != opus_len) {
        LOG_ERR("Read bytes missmatch actual opus len:real  %d  vs  %d", rd, opus_len);
        return OP_MISS;
    }

    if (st->remaining_segments == 0 && st->is_eos_page) {
        return OP_DONE;
    }

    return OP_OK;
}

