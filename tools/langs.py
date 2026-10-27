#!/usr/bin/env python3
"""Список языков из lang/lang_<код>.json - общий для веб-страницы (embed.py) и приложения PKG.

Как скрипт: python3 tools/langs.py <папка с lang_*.json> <выходной файл> - пишет индекс языков
[{"code": "ru", "name": "Русский"}, ...] (приложению PKG нужен именно такой файл: чтение папки
на консоли не используется). Английский и русский идут первыми, остальные по коду.
"""
import glob, json, os, sys


def load_langs(lang_dir):
    langs = []
    for path in glob.glob(os.path.join(lang_dir, 'lang_*.json')):
        code = os.path.basename(path)[5:-5]
        raw = open(path, 'rb').read()
        obj = json.loads(raw.decode('utf-8'))
        langs.append((code, obj.get('_name', code), raw))
    langs.sort(key=lambda x: ({'en': 0, 'ru': 1}.get(x[0], 2), x[0]))
    return langs


def index_bytes(langs):
    return json.dumps([{'code': c, 'name': n} for c, n, _ in langs], ensure_ascii=False).encode('utf-8')


if __name__ == '__main__':
    out = sys.argv[2]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'wb') as f:
        f.write(index_bytes(load_langs(sys.argv[1])))
