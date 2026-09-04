// main/words_data.h —— 由 tools/words_pack.py 生成,勿手改。
#pragma once

#include "lvgl.h"

#define WORDS_GROUP_SIZE 10

typedef struct {
    const char *word;
    const char *gloss;
    const lv_image_dsc_t *img;
} word_entry_t;

extern const word_entry_t WORDS[];
extern const int WORDS_COUNT;

// 发音:G.711 u-law 单声道,WORDS_AUDIO_OFS[i]..[i+1] 是第 i 词的字节区间
extern const uint32_t WORDS_AUDIO_RATE;
extern const uint32_t WORDS_AUDIO_OFS[];
extern const uint8_t  WORDS_AUDIO[];
