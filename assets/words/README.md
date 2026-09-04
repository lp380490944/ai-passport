<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Words App Data

Source data for the `Words` picture-flashcard demo page (`main/demo_words.c`).

## Files

- `words.tsv` — the word bank. One entry per line: `word<TAB>Chinese<TAB>emoji`.
  Entries form practice groups of 10 in line order. The Chinese gloss must stay
  within 26 characters; the emoji selects the entry's picture.
- `ui_strings.txt` — every Chinese character the Words UI renders outside the
  glosses. The font generator unions these with the gloss characters.
- `images/` — 72×72 PNGs cached by the script from Twemoji by emoji codepoint,
  committed to the repository. Delete one to force a re-download.

## Regenerating firmware data

After editing any of the above, run:

```bash
python3 tools/words_pack.py                   # regenerate main/words_data.c/.h
python3 tools/words_pack.py --images          # also fetch pictures, emit main/words_images.c
python3 tools/words_pack.py --font <cjk.otf>  # also regenerate main/font_cjk16.c
```

`--images` needs a Python environment with `pypng` and `lz4` plus `pngquant`
(macOS: `brew install pngquant`); it converts through `tools/LVGLImage.py`
(from LVGL v9.5, MIT) to the I8 format. `--font` needs Node.js
(`npx lv_font_conv`) and a CJK font file. The committed `main/font_cjk16.c`
was generated from Noto Sans CJK SC Regular (SIL OFL 1.1,
https://github.com/notofonts/noto-cjk); the font binary is not stored here.

## Source and license

- Word list and Chinese glosses were written for this project (common concrete
  words for children); same license as the repository.
- Pictures come from [Twemoji](https://github.com/jdecked/twemoji) (graphics
  CC-BY 4.0, copyright Twitter, Inc and other contributors).
