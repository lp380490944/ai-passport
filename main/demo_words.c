// main/demo_words.c —— 像素小熊听音选图:发音 + 三选一 + 积分,NVS 记进度。
//
// 交互(三张图卡自上而下对应三个物理键):
//   上键  短按   选第一张图
//   下键  短按   选第二张图
//   确定  短按   选第三张图
//   上键  长按   再听一遍发音
//   确定  长按   返回菜单(main.c 统一拦截)
//
// 答对 +1 分并自动下一题;答错提示"再试一次"并重放发音。
// 词库在 assets/words/words.tsv,发音/配图/字体由 tools/words_pack.py 生成。
#include "demo.h"
#include "demo_radio.h"   // 复用 NVS 初始化
#include "words_data.h"
#include "ui_pixel.h"
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_FONT_DECLARE(font_cjk16);

static const char *TAG = "words";

#define WORDS_NVS_NS "words"
#define OPTION_COUNT 3

// 小熊配色
#define BEAR_FUR   0x9C6B3F
#define BEAR_DARK  0x5C3A21
#define BEAR_CREAM 0xE8CFA8

// ---- 状态 ----
static int  s_cur;                    // 当前题目的词索引
static int  s_score;                  // 积分
static int  s_correct_slot;           // 正确答案在哪张卡(0..2)
static int  s_options[OPTION_COUNT];  // 三张卡对应的词索引
static bool s_lock;                   // 答对后的过场期间忽略按键
static bool s_dirty;                  // 进度/积分有未落盘修改

// ---- UI 对象 ----
static lv_obj_t   *s_scr, *s_bear, *s_batt;
static lv_obj_t   *s_score_label, *s_total_label, *s_word, *s_feedback;
static lv_obj_t   *s_cards[OPTION_COUNT], *s_imgs[OPTION_COUNT];
static lv_timer_t *s_tick_timer, *s_round_timer;

// ---- 发音:独立任务解码 u-law 并写 codec(按键回调与 LVGL 不做慢活) ----
static TaskHandle_t s_audio_task;
static volatile int s_play_req;       // 词索引+1;0=空闲

static int16_t ulaw_decode(uint8_t u)
{
    u = (uint8_t)~u;
    int t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4);
    return (u & 0x80) ? (int16_t)(0x84 - t) : (int16_t)(t - 0x84);
}

static void play_word(int idx)
{
    if (bsp_audio_set_format(WORDS_AUDIO_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败,跳过发音");
        return;
    }
    bsp_audio_set_volume(85);

    static int16_t chunk[512];
    uint32_t pos = WORDS_AUDIO_OFS[idx];
    uint32_t end = WORDS_AUDIO_OFS[idx + 1];
    while (pos < end) {
        int n = 0;
        while (n < 512 && pos < end) chunk[n++] = ulaw_decode(WORDS_AUDIO[pos++]);
        bsp_audio_write(chunk, (size_t)n * sizeof(int16_t));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    for (;;) {
        int req = s_play_req;
        if (req > 0) { s_play_req = 0; play_word(req - 1); }
        else vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void request_play(int idx) { s_play_req = idx + 1; }

// ---- 持久化 ----

static void progress_save(void)
{
    nvs_handle_t h;
    if (nvs_open(WORDS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败,进度未保存");
        return;
    }
    esp_err_t err = nvs_set_u16(h, "cursor", (uint16_t)s_cur);
    if (err == ESP_OK) err = nvs_set_u16(h, "score", (uint16_t)s_score);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_dirty = false;
    else ESP_LOGW(TAG, "NVS 写入失败: %s", esp_err_to_name(err));
}

static void progress_load(void)
{
    s_cur = 0;
    s_score = 0;
    if (demo_radio_nvs_prepare() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(WORDS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v = 0;
    if (nvs_get_u16(h, "cursor", &v) == ESP_OK && v < WORDS_COUNT) s_cur = v;
    if (nvs_get_u16(h, "score", &v) == ESP_OK) s_score = v;
    nvs_close(h);
}

// ---- 出题与渲染 ----

static void set_card_normal(int slot)
{
    lv_obj_set_style_bg_color(s_cards[slot], lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_border_color(s_cards[slot], lv_color_hex(UI_INK), 0);
}

static void round_start(void)
{
    // 两个不重复的干扰项
    int d1, d2;
    do { d1 = (int)(esp_random() % WORDS_COUNT); } while (d1 == s_cur);
    do { d2 = (int)(esp_random() % WORDS_COUNT); } while (d2 == s_cur || d2 == d1);

    s_correct_slot = (int)(esp_random() % OPTION_COUNT);
    int rest[2] = { d1, d2 }, r = 0;
    for (int i = 0; i < OPTION_COUNT; i++) {
        s_options[i] = (i == s_correct_slot) ? s_cur : rest[r++];
    }

    lv_label_set_text(s_word, WORDS[s_cur].word);
    lv_label_set_text(s_feedback, "听一听 选一选");
    lv_obj_set_style_text_color(s_feedback, lv_color_hex(UI_INK), 0);
    lv_label_set_text_fmt(s_score_label, "分 %d", s_score);
    lv_label_set_text_fmt(s_total_label, "%d/%d", s_cur + 1, WORDS_COUNT);
    for (int i = 0; i < OPTION_COUNT; i++) {
        lv_image_set_src(s_imgs[i], WORDS[s_options[i]].img);
        set_card_normal(i);
    }
    s_lock = false;
    request_play(s_cur);
}

// lv_timer 跑在 LVGL 任务里:刷新电量,并把脏进度批量落盘。
static void tick(lv_timer_t *t)
{
    (void)t;
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_batt, "");     // 读不到就不画,优雅降级
    else         lv_label_set_text_fmt(s_batt, "%d%%", soc);
    lv_obj_set_style_text_color(s_batt,
        (soc >= 0 && soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(0xFFFFFF), 0);

    if (s_dirty) progress_save();
}

static void next_round_cb(lv_timer_t *t)
{
    (void)t;
    lv_timer_delete(s_round_timer);
    s_round_timer = NULL;
    s_cur = (s_cur + 1) % WORDS_COUNT;
    s_dirty = true;
    round_start();
}

static void unflash_cb(lv_timer_t *t)
{
    int slot = (int)(intptr_t)lv_timer_get_user_data(t);
    lv_timer_delete(s_round_timer);
    s_round_timer = NULL;
    set_card_normal(slot);
}

// ---- 像素小熊 ----

static lv_obj_t *bear_block(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static void bear_blink(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void bear_eye(lv_obj_t *bear, int x, int y)
{
    lv_obj_t *eye = bear_block(bear, x, y, 4, 5, UI_INK);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, eye);
    lv_anim_set_exec_cb(&anim, bear_blink);
    lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&anim, 70);
    lv_anim_set_playback_duration(&anim, 70);
    lv_anim_set_repeat_delay(&anim, 2300);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_step);
    lv_anim_start(&anim);
}

// 原创"像素小熊":圆耳、奶油口鼻、鼓肚皮,眨眼 + 答对时跳一下。
static lv_obj_t *bear_create(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *m = lv_obj_create(parent);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(m, x, y);
    lv_obj_set_size(m, 40, 46);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);

    bear_block(m, 4, 0, 10, 8, BEAR_DARK);      // 左耳
    bear_block(m, 26, 0, 10, 8, BEAR_DARK);     // 右耳
    bear_block(m, 7, 2, 4, 4, BEAR_CREAM);
    bear_block(m, 29, 2, 4, 4, BEAR_CREAM);
    bear_block(m, 2, 6, 36, 22, BEAR_FUR);      // 头
    bear_eye(m, 10, 13);
    bear_eye(m, 26, 13);
    bear_block(m, 15, 17, 10, 8, BEAR_CREAM);   // 口鼻
    bear_block(m, 18, 18, 4, 3, UI_INK);        // 鼻子
    bear_block(m, 6, 28, 28, 14, BEAR_FUR);     // 身体
    bear_block(m, 12, 30, 16, 10, BEAR_CREAM);  // 肚皮
    bear_block(m, 7, 42, 10, 4, BEAR_DARK);     // 左脚
    bear_block(m, 23, 42, 10, 4, BEAR_DARK);    // 右脚
    return m;
}

// ---- 生命周期 ----

void demo_words_enter(void)
{
    progress_load();

    s_scr = ui_pixel_screen_create("WORDS");

    // 电量:右上角蓝天空闲区,白云(x188,y8)下方,不遮挡云朵。
    s_batt = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -8, 28);

    // 记分板:左积分,右总进度
    lv_obj_t *stat = ui_pixel_panel_create(s_scr, 11, 50, 218, 28, UI_YELLOW);
    lv_obj_set_style_pad_all(stat, 0, 0);
    s_score_label = ui_pixel_label(stat, "", &font_cjk16, UI_INK);
    lv_obj_align(s_score_label, LV_ALIGN_LEFT_MID, 10, 0);
    s_total_label = ui_pixel_label(stat, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_align(s_total_label, LV_ALIGN_RIGHT_MID, -10, 0);

    // 题目单词
    s_word = ui_pixel_label(s_scr, "", &lv_font_montserrat_20, 0xFFFFFF);
    lv_obj_align(s_word, LV_ALIGN_TOP_MID, 0, 82);

    // 三张选项图卡,自上而下对应 上/下/确定 键
    static const char *KEY_HINTS[OPTION_COUNT] = { LV_SYMBOL_UP, LV_SYMBOL_DOWN, "OK" };
    for (int i = 0; i < OPTION_COUNT; i++) {
        s_cards[i] = ui_pixel_panel_create(s_scr, 12, 108 + i * 60, 190, 54, UI_PAPER);
        lv_obj_set_style_pad_all(s_cards[i], 0, 0);
        lv_obj_t *hint = ui_pixel_label(s_cards[i], KEY_HINTS[i],
                                        &lv_font_montserrat_14, 0x9AA6B0);
        lv_obj_align(hint, LV_ALIGN_LEFT_MID, 8, 0);
        s_imgs[i] = lv_image_create(s_cards[i]);
        lv_image_set_scale(s_imgs[i], 160);            // 72px 图缩到 ~45px
        lv_obj_align(s_imgs[i], LV_ALIGN_CENTER, 8, 0);
    }

    // 小熊站在右下角草地上
    s_bear = bear_create(s_scr, 196, 240);

    // 草地上的反馈行
    s_feedback = ui_pixel_label(s_scr, "", &font_cjk16, UI_INK);
    lv_obj_align(s_feedback, LV_ALIGN_BOTTOM_MID, -20, -8);

    round_start();
    tick(NULL);                                    // 立即显示电量,不等 1 秒
    s_tick_timer = lv_timer_create(tick, 1000, NULL);
    if (!s_audio_task) {
        xTaskCreate(audio_task, "words_audio", 4096, NULL, 4, &s_audio_task);
    }
    lv_screen_load(s_scr);
}

void demo_words_exit(void)
{
    s_play_req = 0;
    if (s_audio_task) { vTaskDelete(s_audio_task); s_audio_task = NULL; }
    if (s_tick_timer)  { lv_timer_delete(s_tick_timer);  s_tick_timer = NULL; }
    if (s_round_timer) { lv_timer_delete(s_round_timer); s_round_timer = NULL; }
    // 退出前把积分与进度落盘。两个 u16 提交只有毫秒级,可接受。
    if (s_dirty) progress_save();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = s_bear = s_batt = NULL;
        s_score_label = s_total_label = s_word = s_feedback = NULL;
        for (int i = 0; i < OPTION_COUNT; i++) s_cards[i] = s_imgs[i] = NULL;
    }
}

// ---- 按键 ----

void demo_words_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_UP) {   // 再听一遍
        request_play(s_cur);
        return;
    }
    if (ev != BSP_BTN_CLICK || s_lock || s_round_timer) return;

    int slot = (int)btn;                             // UP/DOWN/OK 恰为 0/1/2
    if (slot < 0 || slot >= OPTION_COUNT) return;

    if (slot == s_correct_slot) {                    // 答对:加分,过场后下一题
        s_score++;
        s_dirty = true;
        lv_obj_set_style_bg_color(s_cards[slot], lv_color_hex(UI_GRASS), 0);
        lv_obj_set_style_border_color(s_cards[slot], lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text_fmt(s_feedback, "%s 答对啦 +1", WORDS[s_cur].gloss);
        lv_obj_set_style_text_color(s_feedback, lv_color_hex(0x1F5E10), 0);
        lv_label_set_text_fmt(s_score_label, "分 %d", s_score);
        ui_pixel_mascot_jump(s_bear);
        s_lock = true;
        s_round_timer = lv_timer_create(next_round_cb, 1200, NULL);
    } else {                                         // 答错:红一下,重放发音
        lv_obj_set_style_bg_color(s_cards[slot], lv_color_hex(0xF2A9A0), 0);
        lv_obj_set_style_border_color(s_cards[slot], lv_color_hex(UI_RED), 0);
        lv_label_set_text(s_feedback, "再试一次");
        lv_obj_set_style_text_color(s_feedback, lv_color_hex(0x8F1F14), 0);
        request_play(s_cur);
        s_round_timer = lv_timer_create(unflash_cb, 700, (void *)(intptr_t)slot);
    }
}
