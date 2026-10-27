#!/usr/bin/env python3
"""Проверка переводов lang/lang_*.json (общие для веб-страницы и приложения PKG).

  python3 tools/check_lang.py

Проверяет: корректный JSON и только строки; те же ключи, что в lang_en.json (кроме необязательных an_*);
те же {подстановки}; есть _name; каждый символ есть в шрифте приложения (pkg/assets/fonts, нужен fontTools:
pip install fonttools); все ключи, которые просят веб-страница и PKG, существуют в английском файле.
Код возврата 1 при любой ошибке (можно вешать на проверку перед сборкой).
"""
import glob, json, os, re, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
errors = []
def err(m): errors.append(m)

files = {}
for path in sorted(glob.glob(os.path.join(root, 'lang', 'lang_*.json'))):
    code = os.path.basename(path)[5:-5]
    try:
        d = json.load(open(path, encoding='utf-8'))
    except Exception as e:
        err('%s: не JSON: %s' % (code, e)); continue
    if not isinstance(d, dict) or any(not isinstance(v, str) for v in d.values()):
        err('%s: нужен плоский объект со строками' % code); continue
    files[code] = d
if 'en' not in files:
    err('нет lang_en.json'); print('\n'.join(errors)); sys.exit(1)

en = files['en']
ph = lambda s: sorted(re.findall(r'\{[a-zA-Z_]+\}', s))
optional = lambda k: k.startswith('an_')

cmap = None
try:
    from fontTools.ttLib import TTFont
    cmap = TTFont(os.path.join(root, 'pkg', 'assets', 'fonts', 'DejaVuSansMono.ttf')).getBestCmap()
except ImportError:
    print('предупреждение: fontTools не установлен, символы шрифта не проверяются (pip install fonttools)')

for code, d in files.items():
    if not d.get('_name'): err('%s: нет _name' % code)
    for k in en:
        if k not in d and not optional(k): err('%s: нет ключа %s' % (code, k))
    for k, v in d.items():
        if k not in en and not optional(k): err('%s: лишний ключ %s' % (code, k))
        if k in en and ph(v) != ph(en[k]): err('%s: %s: подстановки %s, а в en %s' % (code, k, ph(v), ph(en[k])))
        if k in en and not v.strip(): err('%s: %s пустой' % (code, k))
        if cmap:
            bad = sorted({c for c in v if ord(c) not in cmap and c not in '\n'})
            if bad: err('%s: %s: нет в шрифте: %s' % (code, k, ''.join(bad)))

# Длина (в символах) для мест приложения PKG с фиксированной шириной (шрифт моноширинный, ячейка 12 px).
# Длиннее - текст обрезается многоточием, поэтому переводы должны укладываться.
BUDGET = {
    'id': 3, 'cur': 7, 'worst': 7, 'thr': 7, 'raw': 12,                 # заголовки таблицы SMART
    'act_from': 8, 'act_to': 8, 'act_end': 8, 'act_whole': 17,           # кнопки диапазона проверки
    'hint_start': 17, 'hint_pause': 17, 'hint_resume': 17, 'hint_stop': 17,
    'menu_title': 9,                                                     # кнопка меню в шапке
    'fw': 29, 'serial': 29, 'iface': 29, 'rot': 29, 'hr': 29, 'hw': 29, 'cycles': 29, 'poh': 29,   # подписи слева на экране SMART
}
for code, d in files.items():
    for k, mx in BUDGET.items():
        if k in d and len(d[k]) > mx: err('%s: %s длиннее %d символов (%d): «%s»' % (code, k, mx, len(d[k]), d[k]))

# ключи, которые реально запрашивает код
used = set()
for p in glob.glob(os.path.join(root, 'pkg', 'src', '*.cpp')):
    used |= set(re.findall(r'\btr\("([A-Za-z_0-9]+)"', open(p, encoding='utf-8').read()))
html = open(os.path.join(root, 'web', 'index.html'), encoding='utf-8').read()
used |= set(re.findall(r"\bt\('([A-Za-z_0-9]+)'[,)]", html)) | set(re.findall(r'data-i="([A-Za-z_0-9]+)"', html))
used.discard('_name')
for k in sorted(used):
    if k not in en: err('в коде используется ключ %s, которого нет в lang_en.json' % k)
unused = sorted(k for k in en if k not in used and not k.startswith('_') and not optional(k)
                and not re.match(r'^(v[0-2n]|h[1-7]|m_[a-z_]+|busy|bad_range|err|noconn|lost)$', k))

print('языков: %d (%s)' % (len(files), ', '.join(files)))
if unused: print('ключи без прямого использования (динамические не считаются): ' + ', '.join(unused))
if errors:
    print('\nОШИБКИ (%d):' % len(errors)); print('\n'.join(errors[:80]))
    sys.exit(1)
print('OK')
