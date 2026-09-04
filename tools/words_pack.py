#!/usr/bin/env python3
"""words_pack.py —— 把 assets/words/words.tsv 打包成固件用的词表与字体。

用法:
    python3 tools/words_pack.py                # 只生成 main/words_data.c/.h
    python3 tools/words_pack.py --font FONT.otf  # 同时生成 main/font_cjk16.c
                                               # (需要 node/npx 可用,调用 lv_font_conv)

TSV 格式: 每行 "单词<TAB>释义",按行序每 10 个为一组。
丰富词库 = 编辑 assets/words/words.tsv 后重跑本脚本。
"""
import argparse
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TSV = ROOT / "assets" / "words" / "words.tsv"
UI_STRINGS = ROOT / "assets" / "words" / "ui_strings.txt"
OUT_C = ROOT / "main" / "words_data.c"
OUT_H = ROOT / "main" / "words_data.h"
OUT_FONT = ROOT / "main" / "font_cjk16.c"
GROUP_SIZE = 10
GLOSS_MAX = 26  # 释义超长会在 240px 屏上放不下,提前拦截


def load_entries():
    entries = []
    for ln, line in enumerate(TSV.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 2 or not parts[0] or not parts[1]:
            sys.exit(f"words.tsv:{ln}: 需要 '单词<TAB>释义' 两列: {line!r}")
        word, gloss = parts
        if len(gloss) > GLOSS_MAX:
            sys.exit(f"words.tsv:{ln}: 释义超过 {GLOSS_MAX} 字符: {gloss!r}")
        entries.append((word, gloss))
    if not entries:
        sys.exit("words.tsv 为空")
    return entries


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def emit_c(entries):
    lines = [
        "// main/words_data.c —— 由 tools/words_pack.py 从 assets/words/words.tsv 生成,勿手改。",
        '#include "words_data.h"',
        "",
        "const word_entry_t WORDS[] = {",
    ]
    for word, gloss in entries:
        lines.append(f'    {{ "{c_escape(word)}", "{c_escape(gloss)}" }},')
    lines += [
        "};",
        "",
        "const int WORDS_COUNT = sizeof(WORDS) / sizeof(WORDS[0]);",
        "",
    ]
    OUT_C.write_text("\n".join(lines), encoding="utf-8")

    OUT_H.write_text(
        "\n".join([
            "// main/words_data.h —— 由 tools/words_pack.py 生成,勿手改。",
            "#pragma once",
            "",
            f"#define WORDS_GROUP_SIZE {GROUP_SIZE}",
            "",
            "typedef struct {",
            "    const char *word;",
            "    const char *gloss;",
            "} word_entry_t;",
            "",
            "extern const word_entry_t WORDS[];",
            "extern const int WORDS_COUNT;",
            "",
        ]),
        encoding="utf-8",
    )


def collect_glyphs(entries) -> str:
    chars = set()
    for _, gloss in entries:
        chars.update(gloss)
    if UI_STRINGS.exists():
        chars.update(UI_STRINGS.read_text(encoding="utf-8"))
    # ASCII 由 --range 覆盖;字体只需收非 ASCII 字形
    return "".join(sorted(c for c in chars if ord(c) > 0x7E))


def emit_font(entries, font_path: str):
    glyphs = collect_glyphs(entries)
    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--font", font_path,
        "--size", "16",
        "--bpp", "2",
        "--format", "lvgl",
        "--no-compress",
        "--range", "0x20-0x7E",
        "--symbols", glyphs,
        "--lv-font-name", "font_cjk16",
        "-o", str(OUT_FONT),
    ]
    print(f"生成字体: {len(glyphs)} 个汉字字形 + ASCII")
    subprocess.run(cmd, check=True)
    # 本工程 LVGL 头就是 "lvgl.h",去掉生成文件里的双分支 include 猜测
    text = OUT_FONT.read_text(encoding="utf-8")
    text = text.replace(
        '#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n#include "lvgl.h"\n#else\n#include "lvgl/lvgl.h"\n#endif',
        '#include "lvgl.h"',
    )
    OUT_FONT.write_text(text, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="CJK 字体文件(ttf/otf),给出则重新生成 main/font_cjk16.c")
    args = ap.parse_args()

    entries = load_entries()
    emit_c(entries)
    groups = (len(entries) + GROUP_SIZE - 1) // GROUP_SIZE
    print(f"词表: {len(entries)} 词 / {groups} 组 -> {OUT_C.relative_to(ROOT)}")

    if args.font:
        emit_font(entries, args.font)


if __name__ == "__main__":
    main()
