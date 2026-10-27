#!/usr/bin/env python3
"""Генерирует build/assets_gen.h: страница (с подстановкой версии из version.h) и картинки."""
import re, sys, os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, 'build', 'assets_gen.h')
ver = re.search(r'APP_VERSION\s+"([^"]+)"', open(os.path.join(root, 'src', 'version.h')).read()).group(1)

def arr(name, data):
    lines = []
    for i in range(0, len(data), 24):
        lines.append(','.join(str(b) for b in data[i:i + 24]))
    return 'static const unsigned char %s[] = {\n%s\n};\nstatic const unsigned long %s_LEN = %d;\n' % (
        name, ',\n'.join(lines), name, len(data))

# Переводы: lang/lang_<код>.json - один набор файлов и для веб-страницы, и для приложения PKG.
# Служба отдаёт их по /lang/index.json (список языков) и /lang/<код>.json.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import langs as langs_mod
langs = langs_mod.load_langs(os.path.join(root, 'lang'))
lang_index = langs_mod.index_bytes(langs)

html = open(os.path.join(root, 'web', 'index.html'), encoding='utf-8').read().replace('__VERSION__', ver).encode('utf-8')
os.makedirs(os.path.dirname(out), exist_ok=True)
with open(out, 'w') as f:
    f.write('// Сгенерировано tools/embed.py. Не редактировать.\n#pragma once\n')
    f.write(arr('ASSET_INDEX_HTML', html))
    f.write(arr('ASSET_TITLE_PNG', open(os.path.join(root, 'assets', 'title.png'), 'rb').read()))
    f.write(arr('ASSET_ICON_PNG', open(os.path.join(root, 'assets', 'icon.png'), 'rb').read()))
    f.write(arr('ASSET_QR_PAYPAL_PNG', open(os.path.join(root, 'assets', 'qr_paypal.png'), 'rb').read()))
    f.write(arr('ASSET_QR_PNG', open(os.path.join(root, 'assets', 'qr_sbp.png'), 'rb').read()))
    f.write(arr('ASSET_LANG_INDEX', lang_index))
    for i, (c, n, raw) in enumerate(langs):
        f.write(arr('ASSET_LANG_%d' % i, raw))
    f.write('struct LangAsset { const char* code; const unsigned char* data; unsigned long len; };\n')
    f.write('static const LangAsset ASSET_LANGS[] = {\n')
    for i, (c, n, raw) in enumerate(langs):
        f.write('    {"%s", ASSET_LANG_%d, ASSET_LANG_%d_LEN},\n' % (c, i, i))
    f.write('};\nstatic const int ASSET_LANGS_N = %d;\n' % len(langs))
