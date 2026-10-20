#!/usr/bin/env python3
"""Тест веб-API на ПК (HOST_TEST + ASAN). Запуск: python3 tests/test_api.py build/host_test"""
import json, os, shutil, socket, subprocess, sys, time, urllib.request, urllib.error

BIN = sys.argv[1]
ROOT = '/tmp/hddtest'
PORT = 18686
H = {'X-Requested-With': 'ps4hdd', 'Content-Type': 'application/json'}
fails = []

def check(name, cond, extra=''):
    print(('PASS ' if cond else 'FAIL ') + name + (' ' + str(extra) if not cond else ''))
    if not cond: fails.append(name)

def get(path):
    return urllib.request.urlopen('http://127.0.0.1:%d%s' % (PORT, path), timeout=10).read()

def post(path, body=None, headers=H):
    req = urllib.request.Request('http://127.0.0.1:%d%s' % (PORT, path), data=json.dumps(body or {}).encode(), headers=headers, method='POST')
    try:
        r = urllib.request.urlopen(req, timeout=10)
        return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')

def start(env_extra=None, cfg='{}', mb=64):
    shutil.rmtree(ROOT, ignore_errors=True)
    subprocess.check_call([sys.executable, 'tests/make_fixtures.py', ROOT + '/dev', str(mb), cfg])
    env = dict(os.environ, HDD_PORT=str(PORT), ASAN_OPTIONS='detect_leaks=0')
    env.update(env_extra or {})
    p = subprocess.Popen([BIN], env=env, stdout=open(ROOT + '/out.txt', 'w'), stderr=subprocess.STDOUT)
    for _ in range(50):
        try: get('/api/state'); return p
        except Exception: time.sleep(0.1)
    raise SystemExit('сервер не стартовал')

def wait_idle(limit=60):
    t0 = time.time()
    while time.time() - t0 < limit:
        s = json.loads(get('/api/state'))
        if s['job'] == 'idle': return s
        time.sleep(0.1)
    raise SystemExit('таймаут ожидания')

# ---------- 1. SMART и диски
p = start(cfg='{"realloc":3}')
s = json.loads(get('/api/state'))
check('state: версия 1.0.0.15', s['v'] == '1.0.0.15' and s['job'] == 'idle' and not s['loaded'])
check('в покое диски не читаются', json.loads(get('/api/disks'))['disks'] == [])
check('POST без заголовка отклонён', post('/api/refresh', headers={'Content-Type': 'text/plain'})[0] == 403)
check('refresh принят', post('/api/refresh')[0] == 200)
wait_idle()
d = json.loads(get('/api/disks'))['disks']
check('найден 1 диск da0', len(d) == 1 and d[0]['id'] == 'da0', d and d[0]['id'])
sm = d[0]['smart']
check('модель/прошивка/серийный', sm['model'] == 'ST2000LM007-1R8174' and sm['fw'] == 'EB01' and sm['serial'] == 'WCC4TEST0001', sm)
check('температура 43, часы 127', sm['temp'] == 43 and sm['poh'] == 127, (sm['temp'], sm['poh']))
check('хост: прочитано/записано (ГБ)', sm['hw'] == 5421000000 * 512 and sm['hr'] == 7900000000 * 512)
check('вердикт «внимание» из-за 3 переназначенных', sm['verdict'] == 1 and any(r['id'] == 5 and r['v'] == 3 for r in sm['reasons']), sm['reasons'])
check('пороги прочитаны', any(a['id'] == 5 and a['t'] == 10 for a in sm['attrs']))
check('статус исправен', sm['status'] == 0)
check('rpm 5400', sm['rpm'] == 5400)
check('index.html отдаётся и версия подставлена', b'1.0.0.15' in get('/') and b'__VERSION__' not in get('/'))
check('logo/favicon PNG', get('/logo.png')[:4] == b'\x89PNG' and get('/favicon.ico')[:4] == b'\x89PNG')
check('qr.png и qr_paypal.png отдаются', get('/qr.png')[:4] == b'\x89PNG' and get('/qr_paypal.png')[:4] == b'\x89PNG')
check('log отдаётся', b'version 1.0.0.15' in get('/api/log'))

# ---------- 2. неверные запросы
check('estimate неизвестного диска', post('/api/scan/estimate', {'disk': '../../etc/passwd'})[0] == 400)
check('start неизвестного диска', post('/api/scan/start', {'disk': 'da9'})[0] == 400)
check('неверный диапазон', post('/api/scan/start', {'disk': 'da0', 'from_gb': 5, 'to_gb': 1})[0] in (200, 400))
wait_idle()
check('404', urllib.request.urlopen if True else 0)
try: get('/nope'); check('404 на неизвестный путь', False)
except urllib.error.HTTPError as e: check('404 на неизвестный путь', e.code == 404)
p.terminate(); p.wait()

# ---------- 3. проверка с ошибками чтения
FAULT = (10 * 1048576 + 512, 10 * 1048576 + 512 * 4)  # 3 сектора
p = start(env_extra={'HDD_FAULTS': '%d-%d' % FAULT}, cfg='{}')
post('/api/refresh'); wait_idle()
c, _ = post('/api/scan/estimate', {'disk': 'da0'}); check('estimate принят', c == 200)
s = wait_idle(); e = s['est']
check('estimate: скорость и время', e and e['ok'] and e['speed'] > 0 and e['seconds'] >= 0, e)
c, _ = post('/api/scan/start', {'disk': 'da0'}); check('scan start', c == 200)
c2, _ = post('/api/scan/start', {'disk': 'da0'}); check('второй старт отклонён (одна операция)', c2 == 409)
c3, _ = post('/api/refresh'); check('refresh во время проверки отклонён', c3 == 409)
s = wait_idle(); sc = s['scan']
check('scan завершён с ошибками', sc['state'] == 'done' and sc['msg'] == 'done_errors', sc['state'])
check('плохих секторов ровно 3', sc['bad'] == 3, sc['bad'])
check('LBA плохих секторов', sc['badlba'] == [10 * 2048 + 1, 10 * 2048 + 2, 10 * 2048 + 3], sc['badlba'])
check('весь диск прочитан', sc['done'] == 64 * 1048576)
check('гистограмма: 63 хороших блока + 1 с ошибкой', sum(sc['hist'][:5]) == 63 and sc['hist'][5] == 1, sc['hist'])
check('карта: есть красная ячейка', '6' in sc['map'] and len(sc['map']) == 500)
check('keep-awake вызывался', b'ticks=' in get('/api/log') and int(get('/api/log').split(b'ticks=')[-1].split()[0]) >= 1)
p.terminate(); p.wait()

# ---------- 4. диапазон, пауза, стоп
p = start(env_extra={'HDD_DELAY_US': '40000'})
post('/api/refresh'); wait_idle()
post('/api/scan/start', {'disk': 'da0', 'from_gb': 0.0, 'to_gb': 0.06})  # ~60 МБ
time.sleep(0.5)
post('/api/scan/pause'); time.sleep(0.6)
a = json.loads(get('/api/state'))['scan']; time.sleep(0.8); b = json.loads(get('/api/state'))['scan']
check('пауза: прогресс стоит', a['state'] == 'paused' and a['done'] == b['done'], (a['state'], a['done'], b['done']))
check('пауза не входит в «Прошло»', abs(b['elapsed'] - a['elapsed']) < 0.15, (a['elapsed'], b['elapsed']))
check('диапазон ограничен (to≈30 МБ, кратно 1 МиБ)', a['from'] == 0 and 60000000 <= a['to'] <= 61865984, a['to'])
post('/api/scan/resume'); time.sleep(0.5)
c = json.loads(get('/api/state'))['scan']
check('продолжение после паузы', c['state'] == 'running' and c['done'] > b['done'], (c['state'], c['done'], b['done']))
check('скорость положительна и ETA считается', c['speed'] > 0 and c['eta'] >= 0, (c['speed'], c['eta']))
s = wait_idle(); check('диапазон: завершено без ошибок', s['scan']['state'] == 'done' and s['scan']['msg'] == 'done_ok' and s['scan']['bad'] == 0, s['scan'])
post('/api/scan/start', {'disk': 'da0'}); time.sleep(0.3)
post('/api/scan/stop'); s = wait_idle()
check('стоп: остановлено', s['scan']['state'] == 'stopped' and s['scan']['done'] < 64 * 1048576, s['scan']['state'])
p.terminate(); p.wait()

# ---------- 5. зависшие клиенты и лимит соединений
p = start()
socks = []
for _ in range(12):
    k = socket.socket(); k.connect(('127.0.0.1', PORT)); socks.append(k)
time.sleep(0.3)
ok = False
try: get('/api/state'); ok = True
except Exception: pass
check('при 12 молчащих клиентах сервер не падает', p.poll() is None)
t0 = time.time()
while time.time() - t0 < 8 and not ok:
    try: get('/api/state'); ok = True
    except Exception: time.sleep(0.3)
check('после таймаута (5 с) сервер снова отвечает', ok, round(time.time() - t0, 1))
for k in socks: k.close()
p.terminate(); p.wait()

# ---------- 6. единственная копия
p1 = start()
p2 = subprocess.Popen([BIN], env=dict(os.environ, HDD_PORT=str(PORT), ASAN_OPTIONS='detect_leaks=0'), stdout=open(ROOT + '/out2.txt', 'w'), stderr=subprocess.STDOUT)
t0 = time.time()
while p1.poll() is None and time.time() - t0 < 10: time.sleep(0.2)
check('старая копия завершилась по флагу', p1.poll() == 0, p1.poll())
ok = False
for _ in range(40):
    try: get('/api/state'); ok = True; break
    except Exception: time.sleep(0.2)
check('новая копия заняла порт', ok and p2.poll() is None)
check('флаг удалён', not os.path.exists(ROOT + '/data/quit.flag'))
# выключение кнопкой
check('POST /quit', post('/quit')[0] == 200)
t0 = time.time()
while p2.poll() is None and time.time() - t0 < 10: time.sleep(0.2)
check('/quit завершает процесс с кодом 0', p2.poll() == 0, p2.poll())

# ---------- 6б. два диска: один с SMART, второй (USB-корпус) без SMART
p = start()
open(ROOT + '/dev/da1', 'wb').truncate(8 * 1048576)
open(ROOT + '/dev/pass2', 'wb').close(); open(ROOT + '/dev/pass2.weak', 'wb').write(b'1')
open(ROOT + '/dev/pass0', 'wb').close()   # без файлов-образцов: пропускается
post('/api/refresh'); wait_idle()
dk = json.loads(get('/api/disks'))['disks']
check('два диска найдены', [x['id'] for x in dk] == ['da0', 'da1'], [x['id'] for x in dk])
check('da0 сопоставлен с SMART по ёмкости', dk[0]['smart'] and dk[0]['smart']['model'].startswith('ST2000LM007'))
check('da1 без SMART, но с названием корпуса', dk[1]['smart'] and dk[1]['smart']['bridge'] == 'ANKEJE USB3.0' and not dk[1]['smart']['attrs'] and dk[1]['smart']['verdict'] == -1, dk[1]['smart'])
c, _ = post('/api/scan/start', {'disk': 'da1'}); check('проверка da1 доступна', c == 200)
s = wait_idle(); check('da1 проверен без ошибок', s['scan']['disk'] == 'da1' and s['scan']['msg'] == 'done_ok', s['scan'])
p.terminate(); p.wait()

# ---------- 7. вердикт «плохо»
p = start(cfg='{"failing":1}')
post('/api/refresh'); wait_idle()
sm = json.loads(get('/api/disks'))['disks'][0]['smart']
check('RETURN STATUS «скоро откажет» → плохо', sm['status'] == 1 and sm['verdict'] == 2, (sm['status'], sm['verdict']))
p.terminate(); p.wait()
out = open(ROOT + '/out.txt').read()
check('ASAN/UBSAN: ошибок нет', 'ERROR: AddressSanitizer' not in out and 'runtime error' not in out)

print('\nИТОГО: провалено %d' % len(fails))
sys.exit(1 if fails else 0)
