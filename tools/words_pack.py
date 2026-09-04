#!/usr/bin/env python3
"""words_pack.py —— 把 assets/words/words.tsv 打包成固件用的词表、配图与字体。

用法:
    python3 tools/words_pack.py                # 只生成 main/words_data.c/.h
    python3 tools/words_pack.py --images       # 同时下载 Twemoji 并生成 main/words_images.c
                                               # (需要 pypng+lz4 的 Python 环境和 pngquant)
    python3 tools/words_pack.py --font FONT.otf  # 同时生成 main/font_cjk16.c
                                               # (需要 node/npx 可用,调用 lv_font_conv)

TSV 格式: 每行 "单词<TAB>中文<TAB>emoji",按行序每 10 个为一组。
丰富词库 = 编辑 assets/words/words.tsv 后重跑本脚本(新词要重跑 --images 和 --font)。
配图来自 Twemoji(CC-BY 4.0),按 emoji 码点从 jdecked/twemoji 下载并缓存在
assets/words/images/,再由 tools/LVGLImage.py 转成 I8 格式的 LVGL C 数组。
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent.parent
TSV = ROOT / "assets" / "words" / "words.tsv"
UI_STRINGS = ROOT / "assets" / "words" / "ui_strings.txt"
IMAGES_DIR = ROOT / "assets" / "words" / "images"
IMG_TOOL = ROOT / "tools" / "LVGLImage.py"
OUT_C = ROOT / "main" / "words_data.c"
OUT_H = ROOT / "main" / "words_data.h"
OUT_IMG = ROOT / "main" / "words_images.c"
OUT_FONT = ROOT / "main" / "font_cjk16.c"
GROUP_SIZE = 10
GLOSS_MAX = 26  # 释义超长会在 240px 屏上放不下,提前拦截
TWEMOJI_URL = "https://raw.githubusercontent.com/jdecked/twemoji/main/assets/72x72/{code}.png"


def load_entries():
    entries = []
    for ln, line in enumerate(TSV.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 3 or not all(parts):
            sys.exit(f"words.tsv:{ln}: 需要 '单词<TAB>中文<TAB>emoji' 三列: {line!r}")
        word, gloss, emoji = parts
        if len(gloss) > GLOSS_MAX:
            sys.exit(f"words.tsv:{ln}: 释义超过 {GLOSS_MAX} 字符: {gloss!r}")
        entries.append((word, gloss, emoji))
    if not entries:
        sys.exit("words.tsv 为空")
    return entries


def c_escape(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def img_symbol(idx: int) -> str:
    return f"img_w{idx:03d}"


def emit_c(entries):
    lines = [
        "// main/words_data.c —— 由 tools/words_pack.py 从 assets/words/words.tsv 生成,勿手改。",
        '#include "words_data.h"',
        "",
    ]
    for i in range(len(entries)):
        lines.append(f"extern const lv_image_dsc_t {img_symbol(i)};")
    lines += ["", "const word_entry_t WORDS[] = {"]
    for i, (word, gloss, _emoji) in enumerate(entries):
        lines.append(
            f'    {{ "{c_escape(word)}", "{c_escape(gloss)}", &{img_symbol(i)} }},')
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
            '#include "lvgl.h"',
            "",
            f"#define WORDS_GROUP_SIZE {GROUP_SIZE}",
            "",
            "typedef struct {",
            "    const char *word;",
            "    const char *gloss;",
            "    const lv_image_dsc_t *img;",
            "} word_entry_t;",
            "",
            "extern const word_entry_t WORDS[];",
            "extern const int WORDS_COUNT;",
            "",
        ]),
        encoding="utf-8",
    )


def emoji_code(emoji: str) -> str:
    # Twemoji 文件名:码点小写十六进制用 - 连接,变体选择符 FE0F 不参与
    return "-".join(f"{ord(c):x}" for c in emoji if ord(c) != 0xFE0F)


def fetch_images(entries):
    IMAGES_DIR.mkdir(parents=True, exist_ok=True)
    paths = []
    for i, (word, _gloss, emoji) in enumerate(entries):
        slug = word.replace(" ", "-")
        path = IMAGES_DIR / f"w{i:03d}_{slug}.png"
        paths.append(path)
        if path.exists():
            continue
        url = TWEMOJI_URL.format(code=emoji_code(emoji))
        print(f"下载 {word} {emoji} <- {url}")
        try:
            with urllib.request.urlopen(url) as resp:
                path.write_bytes(resp.read())
        except Exception as e:
            sys.exit(f"下载失败 {word} {emoji}: {e}")
    return paths


def emit_images(entries):
    paths = fetch_images(entries)
    chunks = [
        "// main/words_images.c —— 由 tools/words_pack.py 生成(Twemoji, CC-BY 4.0),勿手改。",
        '#include "lvgl.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for i, png in enumerate(paths):
            sym = img_symbol(i)
            # RGB565 是本机屏幕原生格式,渲染路径最稳;透明背景直接合成到
            # 卡片纸色(UI_PAPER)上,避免依赖索引色 alpha 混合。
            subprocess.run(
                [sys.executable, str(IMG_TOOL), "--ofmt", "C", "--cf", "RGB565",
                 "--rgb565dither", "--background", "0xF4F4EA",
                 "--compress", "NONE", "--name", sym, "-o", tmp, str(png)],
                check=True, capture_output=True)
            text = (pathlib.Path(tmp) / f"{sym}.c").read_text(encoding="utf-8")
            # 去掉每个文件的 include 头,只保留符号定义部分
            marker = text.index("#ifndef LV_ATTRIBUTE_IMG")
            chunks.append(text[marker:])
    OUT_IMG.write_text("\n".join(chunks), encoding="utf-8")
    size = OUT_IMG.stat().st_size
    print(f"配图: {len(paths)} 张 -> {OUT_IMG.relative_to(ROOT)} ({size // 1024} KB 源码)")


def collect_glyphs(entries) -> str:
    chars = set()
    for _, gloss, _ in entries:
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
    ap.add_argument("--images", action="store_true",
                    help="下载 Twemoji 并重新生成 main/words_images.c")
    ap.add_argument("--font", help="CJK 字体文件(ttf/otf),给出则重新生成 main/font_cjk16.c")
    args = ap.parse_args()

    entries = load_entries()
    emit_c(entries)
    groups = (len(entries) + GROUP_SIZE - 1) // GROUP_SIZE
    print(f"词表: {len(entries)} 词 / {groups} 组 -> {OUT_C.relative_to(ROOT)}")

    if args.images:
        emit_images(entries)
    if args.font:
        emit_font(entries, args.font)


if __name__ == "__main__":
    main()
