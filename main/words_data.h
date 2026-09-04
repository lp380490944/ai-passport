// main/words_data.h —— 由 tools/words_pack.py 生成,勿手改。
#pragma once

#define WORDS_GROUP_SIZE 10

typedef struct {
    const char *word;
    const char *gloss;
} word_entry_t;

extern const word_entry_t WORDS[];
extern const int WORDS_COUNT;
