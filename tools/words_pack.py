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
            "// 发音:G.711 u-law 单声道,WORDS_AUDIO_OFS[i]..[i+1] 是第 i 词的字节区间",
            "extern const uint32_t WORDS_AUDIO_RATE;",
            "extern const uint32_t WORDS_AUDIO_OFS[];",
            "extern const uint8_t  WORDS_AUDIO[];",
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
            # I8 索引色省 40% 空间(给发音留余量);透明背景直接合成到
            # 卡片纸色(UI_PAPER)上,不依赖 alpha 混合。
            subprocess.run(
                [sys.executable, str(IMG_TOOL), "--ofmt", "C", "--cf", "I8",
                 "--background", "0xF4F4EA",
                 "--compress", "NONE", "--name", sym, "-o", tmp, str(png)],
                check=True, capture_output=True)
            text = (pathlib.Path(tmp) / f"{sym}.c").read_text(encoding="utf-8")
            # 去掉每个文件的 include 头,只保留符号定义部分
            marker = text.index("#ifndef LV_ATTRIBUTE_IMG")
            chunks.append(text[marker:])
    OUT_IMG.write_text("\n".join(chunks), encoding="utf-8")
    size = OUT_IMG.stat().st_size
    print(f"配图: {len(paths)} 张 -> {OUT_IMG.relative_to(ROOT)} ({size // 1024} KB 源码)")


OUT_AUDIO = ROOT / "main" / "words_audio.c"
AUDIO_RATE = 8000
AUDIO_VOICE = "Samantha"


def _ulaw_byte(sample: int) -> int:
    # G.711 μ-law 编码,单样本
    BIAS, CLIP = 0x84, 32635
    sign = 0x80 if sample < 0 else 0
    if sample < 0:
        sample = -sample
    if sample > CLIP:
        sample = CLIP
    sample += BIAS
    exp, mask = 7, 0x4000
    while exp > 0 and not sample & mask:
        exp -= 1
        mask >>= 1
    mantissa = (sample >> (exp + 3)) & 0x0F
    return ~(sign | (exp << 4) | mantissa) & 0xFF


def _tts_ulaw(word: str, tmp: pathlib.Path) -> bytes:
    import wave
    aiff = tmp / "w.aiff"
    wav = tmp / "w.wav"
    subprocess.run(["say", "-v", AUDIO_VOICE, "-o", str(aiff), word], check=True)
    subprocess.run(["afconvert", str(aiff), "-f", "WAVE",
                    "-d", f"LEI16@{AUDIO_RATE}", "-c", "1", str(wav)], check=True)
    with wave.open(str(wav), "rb") as f:
        raw = f.readframes(f.getnframes())
    samples = [int.from_bytes(raw[i:i + 2], "little", signed=True)
               for i in range(0, len(raw), 2)]
    # 掐头去尾的静音,两端各留 60ms
    thresh, pad = 400, AUDIO_RATE * 60 // 1000
    idx = [i for i, s in enumerate(samples) if abs(s) > thresh]
    if idx:
        samples = samples[max(0, idx[0] - pad):idx[-1] + pad]
    return bytes(_ulaw_byte(s) for s in samples)


def emit_audio(entries):
    blob = bytearray()
    offsets = [0]
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = pathlib.Path(tmpdir)
        for word, _gloss, _emoji in entries:
            data = _tts_ulaw(word, tmp)
            blob.extend(data)
            offsets.append(len(blob))
            print(f"发音 {word}: {len(data) * 1000 // AUDIO_RATE} ms")
    lines = [
        "// main/words_audio.c —— 由 tools/words_pack.py 生成(macOS say 合成,G.711 u-law),勿手改。",
        '#include "words_data.h"',
        "",
        f"const uint32_t WORDS_AUDIO_RATE = {AUDIO_RATE};",
        "",
        "const uint32_t WORDS_AUDIO_OFS[] = {",
    ]
    lines.append("    " + ",".join(str(o) for o in offsets))
    lines += ["};", "", "const uint8_t WORDS_AUDIO[] = {"]
    for i in range(0, len(blob), 24):
        lines.append("    " + ",".join(f"0x{b:02x}" for b in blob[i:i + 24]) + ",")
    lines += ["};", ""]
    OUT_AUDIO.write_text("\n".join(lines), encoding="utf-8")
    print(f"发音: {len(entries)} 词 / {len(blob) // 1024} KB u-law -> {OUT_AUDIO.relative_to(ROOT)}")


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
    ap.add_argument("--audio", action="store_true",
                    help="用 macOS say+afconvert 重新生成 main/words_audio.c")
    ap.add_argument("--font", help="CJK 字体文件(ttf/otf),给出则重新生成 main/font_cjk16.c")
    args = ap.parse_args()

    entries = load_entries()
    emit_c(entries)
    groups = (len(entries) + GROUP_SIZE - 1) // GROUP_SIZE
    print(f"词表: {len(entries)} 词 / {groups} 组 -> {OUT_C.relative_to(ROOT)}")

    if args.images:
        emit_images(entries)
    if args.audio:
        emit_audio(entries)
    if args.font:
        emit_font(entries, args.font)


if __name__ == "__main__":
    main()
