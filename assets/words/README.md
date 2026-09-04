<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Words App Data

Source data for the `Words` flashcard demo page (`main/demo_words.c`).

## Files

- `words.tsv` — the word bank. One entry per line: `word<TAB>Chinese gloss`.
  Entries form practice groups of 10 in line order. Glosses must stay within
  26 characters so they fit the 240 px card.
- `ui_strings.txt` — every Chinese character the Words UI renders outside the
  glosses. The font generator unions these with the gloss characters.

## Regenerating firmware data

After editing either file, run:

```bash
python3 tools/words_pack.py                 # regenerate main/words_data.c/.h
python3 tools/words_pack.py --font <cjk.otf>  # also regenerate main/font_cjk16.c
```

The `--font` step needs Node.js (`npx lv_font_conv`) and a CJK font file.
The committed `main/font_cjk16.c` was generated from Noto Sans CJK SC Regular
(SIL Open Font License 1.1, https://github.com/notofonts/noto-cjk). The font
binary itself is not stored in this repository; download it from that project
when regenerating.

## Source and license

Word list and glosses were written for this project (common CET-4 core
vocabulary); same license as the repository.
