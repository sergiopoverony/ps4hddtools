#!/usr/bin/env python3
"""Создаёт «диск» (обычный файл) и образцы ответов SAT для теста HOST_TEST."""
import os, struct, sys
d = sys.argv[1]
os.makedirs(d, exist_ok=True)
DISK_MB = int(sys.argv[2]) if len(sys.argv) > 2 else 64
with open(os.path.join(d, 'da0'), 'wb') as f:
    f.truncate(DISK_MB * 1024 * 1024)
open(os.path.join(d, 'pass1'), 'wb').close()
sectors = DISK_MB * 1024 * 1024 // 512

def ident_str(s, words):
    s = s.ljust(words * 2).encode()
    out = bytearray()
    for i in range(0, len(s), 2):
        out += bytes([s[i + 1], s[i]])
    return bytes(out)

idb = bytearray(512)
idb[20:40] = ident_str('WCC4TEST0001', 10)          # слова 10..19 серийный номер
idb[46:54] = ident_str('EB01', 4)                   # слова 23..26 прошивка
idb[54:94] = ident_str('ST2000LM007-1R8174', 20)    # слова 27..46 модель
idb[200:208] = struct.pack('<Q', sectors)           # слова 100..103
idb[434:436] = struct.pack('<H', 5400)
open(os.path.join(d, 'pass1.identify'), 'wb').write(idb)

def fix(b):
    b[511] = (-sum(b[:511])) & 0xFF
    return bytes(b)

def attr(buf, i, aid, cur, worst, raw):
    o = 2 + i * 12
    buf[o] = aid; buf[o + 1] = 0x33; buf[o + 3] = cur; buf[o + 4] = worst
    buf[o + 5:o + 11] = raw.to_bytes(6, 'little')

import json
cfg = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
sm = bytearray(512); th = bytearray(512)
attrs = [(1, 100, 99, 0, 6), (5, 100, 100, cfg.get('realloc', 0), 10), (9, 99, 99, 127, 0), (12, 100, 100, 98, 20),
         (0xC2, 57, 40, 43, 0), (0xC4, 100, 100, 0, 30), (0xC5, 100, 100, cfg.get('pending', 0), 0),
         (0xC7, 200, 200, 0, 0), (0xF1, 100, 253, 5421000000, 0), (0xF2, 100, 253, 7900000000, 0)]
for i, (aid, c, w, raw, t) in enumerate(attrs):
    attr(sm, i, aid, c, w, raw)
    th[2 + i * 12] = aid; th[2 + i * 12 + 1] = t
open(os.path.join(d, 'pass1.smart'), 'wb').write(fix(sm))
open(os.path.join(d, 'pass1.thresh'), 'wb').write(fix(th))
open(os.path.join(d, 'pass1.status'), 'wb').write(bytes([0xF4, 0x2C]) if cfg.get('failing') else bytes([0x4F, 0xC2]))
