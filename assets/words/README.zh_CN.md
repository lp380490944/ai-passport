<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 单词应用数据

`Words` 儿童看图单词演示页(`main/demo_words.c`)的源数据。

## 文件

- `words.tsv` — 词库。每行一条:`单词<TAB>中文<TAB>emoji`。按行序每 10 条为一个
  练习小组。中文不超过 26 个字符;emoji 决定这条词的配图。
- `ui_strings.txt` — 单词界面在词义之外要渲染的全部汉字。字体生成时会与
  词义用字取并集。
- `images/` — 由脚本按 emoji 码点从 Twemoji 下载缓存的 72×72 PNG,已入库。
  删除某张可强制下次重新下载。

## 重新生成固件数据

编辑上述文件后运行:

```bash
python3 tools/words_pack.py                   # 重新生成 main/words_data.c/.h
python3 tools/words_pack.py --images          # 同时下载配图并生成 main/words_images.c
python3 tools/words_pack.py --audio           # 同时合成发音并生成 main/words_audio.c
python3 tools/words_pack.py --font <cjk.otf>  # 同时重新生成 main/font_cjk16.c
```

`--audio` 仅限 macOS:用系统 `say`(Samantha 声音)逐词合成,`afconvert` 转
8 kHz 16 bit 单声道,再按 G.711 u-law 压缩打进固件。

`--images` 需要装了 `pypng`、`lz4` 的 Python 环境和 `pngquant`
(macOS: `brew install pngquant`),内部调用 `tools/LVGLImage.py`(取自 LVGL
v9.5,MIT)转成 I8 格式。`--font` 需要 Node.js(`npx lv_font_conv`)和一个
CJK 字体文件。仓库中已提交的 `main/font_cjk16.c` 由 Noto Sans CJK SC Regular
生成(SIL OFL 1.1,https://github.com/notofonts/noto-cjk),字体二进制不入库。

## 来源与授权

- 词表与中文词义为本项目编写(儿童常见具象词),授权同仓库。
- 配图来自 [Twemoji](https://github.com/jdecked/twemoji)(图形 CC-BY 4.0,
  版权归 Twitter, Inc 及其他贡献者)。
