# Translations / Переводы

One set of files is used by **both** the web page (served by the payload at `/lang/<code>.json`) and the PKG app (copied into the package by `pkg/Makefile`).

Один набор файлов используется и веб-страницей (служба отдаёт их по `/lang/<код>.json`), и приложением PKG (`pkg/Makefile` кладёт их в пакет).

## Add a language / Как добавить язык

1. Copy `lang_en.json` to `lang_<code>.json`. The code is lower-case: `de`, `pt`, `no`, `pt-br`.
   The PKG app picks the file by the console's system language (`pkg/src/i18n.cpp`, `lang_candidates()`): for example `pt-br` is tried first for Brazilian Portuguese and then `pt`.
2. Translate the **values**. Never change the keys.
3. Set `_name` to the language's own name (`"Deutsch"`); it is shown in the language pickers.
4. Run `python3 tools/check_lang.py`. It checks that:
   - the JSON is valid and flat;
   - the keys are the same as in `lang_en.json` (the `an_*` keys, names of SMART attributes, are optional: without them the drive's own names are shown);
   - `{placeholders}` such as `{d}`, `{gb}`, `{time}` are kept exactly;
   - every character exists in the PKG font (DejaVu Sans Mono: Latin, Greek, Cyrillic; no Arabic, Hebrew, Thai, Vietnamese, CJK);
   - short strings fit the fixed-width places of the PKG app (table headers, range buttons, the menu button);
   - every key used by the code exists in `lang_en.json`.
5. Rebuild. Nothing else has to be edited: the web page and the app discover the file by themselves.

## Notes / Заметки

- Keys starting with `h` + digit (`h1`..`h7`), `v0..v2`, `m_*` are looked up dynamically by the code.
- Some texts exist in two forms on purpose: `cWarn` (web, long) and `cWarnShort` (PKG, short dialog); `vn` (web, "No data") and `vnSmart` (PKG, "No SMART").
- English is always loaded first as the fallback, so a key missing in a translation shows English text instead of an empty string.
- The service log (`/api/log`) is not translated.
