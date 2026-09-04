// main/demo_words.c —— 像素小熊背单词:分组翻卡复习,NVS 记忆进度。
//
// 交互:
//   上/下 短按   上一词 / 下一词(组末下翻弹出"小组完成")
//   确定  短按   翻出 / 收起中文释义
//   确定  长按   返回菜单(main.c 统一拦截)
//
// 词库在 assets/words/words.tsv,每 10 词一组;编辑后跑
// tools/words_pack.py 重新生成词表与中文字体。
#include "demo.h"
#include "demo_radio.h"   // 复用 NVS 初始化
#include "words_data.h"
#include "ui_pixel.h"
#include "bsp_battery.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

LV_FONT_DECLARE(font_cjk16);

static const char *TAG = "words";

#define WORDS_NVS_NS  "words"
#define WORDS_NVS_KEY "cursor"

// 小熊配色
#define BEAR_FUR   0x9C6B3F
#define BEAR_DARK  0x5C3A21
#define BEAR_CREAM 0xE8CFA8

// ---- 状态 ----
static int  s_cur;               // 当前词索引
static bool s_show_gloss;        // 释义是否翻开
static bool s_done_overlay;      // "小组完成"覆盖层是否在显示
static bool s_dirty;             // 游标有未落盘修改

// ---- UI 对象 ----
static lv_obj_t   *s_scr, *s_bear, *s_batt;
static lv_obj_t   *s_group_label, *s_total_label;
static lv_obj_t   *s_img, *s_word, *s_gloss;
static lv_obj_t   *s_dots[WORDS_GROUP_SIZE];
static lv_obj_t   *s_done_panel, *s_done_title, *s_done_sub;
static lv_timer_t *s_timer;

// ---- 持久化 ----

static void cursor_save(void)
{
    nvs_handle_t h;
    if (nvs_open(WORDS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败,进度未保存");
        return;
    }
    esp_err_t err = nvs_set_u16(h, WORDS_NVS_KEY, (uint16_t)s_cur);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_dirty = false;
    else ESP_LOGW(TAG, "NVS 写入失败: %s", esp_err_to_name(err));
}

static void cursor_load(void)
{
    s_cur = 0;
    if (demo_radio_nvs_prepare() != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open(WORDS_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t v = 0;
    if (nvs_get_u16(h, WORDS_NVS_KEY, &v) == ESP_OK && v < WORDS_COUNT) {
        s_cur = v;
    }
    nvs_close(h);
}

// ---- 渲染 ----

static void refresh(void)
{
    int group = s_cur / WORDS_GROUP_SIZE;
    int pos   = s_cur % WORDS_GROUP_SIZE;
    int total_groups = (WORDS_COUNT + WORDS_GROUP_SIZE - 1) / WORDS_GROUP_SIZE;

    lv_label_set_text_fmt(s_group_label, "第%d组/%d", group + 1, total_groups);
    lv_label_set_text_fmt(s_total_label, "%d/%d", s_cur + 1, WORDS_COUNT);

    lv_image_set_src(s_img, WORDS[s_cur].img);
    lv_label_set_text(s_word, WORDS[s_cur].word);
    if (s_show_gloss) {
        lv_label_set_text(s_gloss, WORDS[s_cur].gloss);
        lv_obj_set_style_text_color(s_gloss, lv_color_hex(UI_INK), 0);
    } else {
        lv_label_set_text(s_gloss, "按 OK 看释义");
        lv_obj_set_style_text_color(s_gloss, lv_color_hex(0x9AA6B0), 0);
    }

    // 组内进度点:走过=草绿,当前=黄,未到=白
    int group_len = WORDS_COUNT - group * WORDS_GROUP_SIZE;
    if (group_len > WORDS_GROUP_SIZE) group_len = WORDS_GROUP_SIZE;
    for (int i = 0; i < WORDS_GROUP_SIZE; i++) {
        if (i >= group_len) { lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        uint32_t c = (i < pos) ? UI_GRASS : (i == pos) ? UI_YELLOW : 0xFFFFFF;
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(c), 0);
    }

    if (s_done_overlay) lv_obj_remove_flag(s_done_panel, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_add_flag(s_done_panel, LV_OBJ_FLAG_HIDDEN);
}

static void show_done(bool all_done)
{
    s_done_overlay = true;
    lv_label_set_text(s_done_title, all_done ? "全部完成!" : "小组完成!");
    lv_label_set_text(s_done_sub, all_done ? "再来一遍" : "按任意键继续");
    ui_pixel_mascot_jump(s_bear);
    refresh();
}

// lv_timer 跑在 LVGL 任务里:刷新电量,并把脏游标批量落盘
// (避免在按键回调所在的 button 任务里做 flash 写入)。
static void tick(lv_timer_t *t)
{
    (void)t;
    int soc = bsp_battery_soc();
    if (soc < 0) lv_label_set_text(s_batt, "");     // 读不到就不画,优雅降级
    else         lv_label_set_text_fmt(s_batt, "%d%%", soc);
    lv_obj_set_style_text_color(s_batt,
        (soc >= 0 && soc < 20) ? lv_color_hex(UI_RED) : lv_color_hex(0xFFFFFF), 0);

    if (s_dirty) cursor_save();
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

// 原创"像素小熊":圆耳、奶油口鼻、鼓肚皮,眨眼 + 切词时跳一下。
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
    cursor_load();
    s_show_gloss = false;
    s_done_overlay = false;

    s_scr = ui_pixel_screen_create("WORDS");

    // 电量:右上角蓝天空闲区,白云(x188,y8)下方,不遮挡云朵。
    s_batt = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -8, 28);

    // 进度面板:左"第N组/共几组",右"当前/总词数"
    lv_obj_t *stat = ui_pixel_panel_create(s_scr, 11, 50, 218, 30, UI_YELLOW);
    lv_obj_set_style_pad_all(stat, 0, 0);
    s_group_label = ui_pixel_label(stat, "", &font_cjk16, UI_INK);
    lv_obj_align(s_group_label, LV_ALIGN_LEFT_MID, 10, 0);
    s_total_label = ui_pixel_label(stat, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_align(s_total_label, LV_ALIGN_RIGHT_MID, -10, 0);

    // 单词卡:大图在上给小朋友认,英文在中,中文默认藏着
    lv_obj_t *card = ui_pixel_panel_create(s_scr, 18, 86, 204, 142, UI_PAPER);
    lv_obj_set_style_pad_all(card, 0, 0);
    s_img = lv_image_create(card);
    lv_obj_align(s_img, LV_ALIGN_TOP_MID, 0, 8);
    s_word = ui_pixel_label(card, "", &lv_font_montserrat_20, UI_INK);
    lv_obj_align(s_word, LV_ALIGN_TOP_MID, 0, 86);
    s_gloss = ui_pixel_label(card, "", &font_cjk16, UI_INK);
    lv_obj_set_style_text_align(s_gloss, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_gloss, LV_ALIGN_TOP_MID, 0, 114);

    // 组内 10 词进度点
    for (int i = 0; i < WORDS_GROUP_SIZE; i++) {
        lv_obj_t *dot = lv_obj_create(s_scr);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(dot, 42 + i * 16, 234);
        lv_obj_set_size(dot, 12, 8);
        lv_obj_set_style_radius(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(dot, 2, 0);
        s_dots[i] = dot;
    }

    // 草地提示 + 小熊
    lv_obj_t *hint = ui_pixel_label(s_scr,
        "上下切词  OK 看释义", &font_cjk16, UI_INK);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -7);
    s_bear = bear_create(s_scr, 100, 250);

    // "小组完成"覆盖层(默认隐藏)
    s_done_panel = ui_pixel_panel_create(s_scr, 35, 116, 170, 84, UI_ORANGE);
    lv_obj_set_style_pad_all(s_done_panel, 0, 0);
    s_done_title = ui_pixel_label(s_done_panel, "", &font_cjk16, UI_INK);
    lv_obj_align(s_done_title, LV_ALIGN_TOP_MID, 0, 16);
    s_done_sub = ui_pixel_label(s_done_panel, "", &font_cjk16, UI_INK);
    lv_obj_align(s_done_sub, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_add_flag(s_done_panel, LV_OBJ_FLAG_HIDDEN);

    refresh();
    tick(NULL);                                    // 立即显示电量,不等 1 秒
    s_timer = lv_timer_create(tick, 1000, NULL);
    lv_screen_load(s_scr);
}

void demo_words_exit(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    // 退出前把游标落盘。u16 提交只有毫秒级,可接受。
    if (s_dirty) cursor_save();
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = s_bear = s_batt = NULL;
        s_group_label = s_total_label = s_img = s_word = s_gloss = NULL;
        s_done_panel = s_done_title = s_done_sub = NULL;
        for (int i = 0; i < WORDS_GROUP_SIZE; i++) s_dots[i] = NULL;
    }
}

// ---- 按键 ----

static void goto_word(int idx)
{
    s_cur = idx;
    s_show_gloss = false;
    s_dirty = true;
    ui_pixel_mascot_jump(s_bear);
    refresh();
}

void demo_words_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev != BSP_BTN_CLICK) return;

    if (s_done_overlay) {                    // 任意键关闭覆盖层,进入下一组
        s_done_overlay = false;
        goto_word((s_cur + 1) % WORDS_COUNT);
        return;
    }

    if (btn == BSP_BTN_UP) {
        goto_word((s_cur + WORDS_COUNT - 1) % WORDS_COUNT);
    } else if (btn == BSP_BTN_DOWN) {
        if ((s_cur + 1) % WORDS_GROUP_SIZE == 0 || s_cur + 1 == WORDS_COUNT) {
            show_done(s_cur + 1 == WORDS_COUNT);  // 组末/词库末弹庆祝,下一键翻组
        } else {
            goto_word(s_cur + 1);
        }
    } else if (btn == BSP_BTN_OK) {
        s_show_gloss = !s_show_gloss;
        if (s_show_gloss) ui_pixel_mascot_jump(s_bear);
        refresh();
    }
}
