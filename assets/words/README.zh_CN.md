<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 单词应用数据

`Words` 背单词演示页(`main/demo_words.c`)的源数据。

## 文件

- `words.tsv` — 词库。每行一条:`单词<TAB>中文释义`。按行序每 10 条为一个
  练习小组。释义不超过 26 个字符,否则 240 px 卡片放不下。
- `ui_strings.txt` — 单词界面在释义之外要渲染的全部汉字。字体生成时会与
  释义用字取并集。

## 重新生成固件数据

编辑上述文件后运行:

```bash
python3 tools/words_pack.py                 # 重新生成 main/words_data.c/.h
python3 tools/words_pack.py --font <cjk.otf>  # 同时重新生成 main/font_cjk16.c
```

`--font` 一步需要 Node.js(`npx lv_font_conv`)和一个 CJK 字体文件。
仓库中已提交的 `main/font_cjk16.c` 由 Noto Sans CJK SC Regular 生成
(SIL Open Font License 1.1,https://github.com/notofonts/noto-cjk)。
字体二进制不入库,重新生成时自行到该项目下载。

## 来源与授权

词表与释义为本项目编写(CET-4 常用核心词),授权同仓库。
