#include "ui.h"
#include "service.h"
#include "scene.h"
#include "image.h"
#include "i18n.h"
#include "log.h"

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/ImeDialog.h>
#include <orbis/CommonDialog.h>
#include <orbis/Sysmodule.h>

extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceSystemServiceHideSplashScreen(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int FONT_PX   = 20;   // было 26 - уменьшили по просьбе пользователя, проверить на экране
static const int MARGIN_X  = 40;
static const int BANNER_LEFT = 12;
static const int BANNER_TOP = 16;
static const int BANNER_H   = 64;

static const Color C_BG    = { 22,  24,  30 };
static const Color C_ROW2  = { 28,  31,  39 };
static const Color C_TEXT  = { 230, 230, 230 };
static const Color C_DIM   = { 150, 155, 165 };
static const Color C_DIMMER = { 95,  99,  108 };
static const Color C_GREEN = { 110, 210, 130 };
static const Color C_AMBER = { 235, 180,  80 };
static const Color C_RED   = { 235, 100, 100 };
static const Color C_PANEL = { 40,  44,  56 };
static const Color C_EDGE  = { 110, 120, 150 };

static Scene2D* g_scene = NULL;
static FT_Face  g_font;
static int      g_cellW = 16;
static bool     g_ready = false;
static int      g_redraw = 2;
static int      g_frameID = 0;
static Image    g_banner;
static bool     g_bannerOk = false;
static int      g_bannerW  = 0;

// QR-коды для донатов (как в меню веб-интерфейса) - грузятся по требованию, при первом открытии.
static Image    g_qrSbp, g_qrPaypal;
static bool     g_qrSbpOk = false, g_qrPaypalOk = false, g_qrTried = false;
static bool     g_showDonate = false;

// ---------------------------------------------------------------- UTF-8

static int decode_utf8(const unsigned char*& p)
{
    unsigned c = *p++;
    if (c < 0x80) return (int)c;

    int extra;
    unsigned cp;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else return '?';

    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) return '?';
        cp = (cp << 6) | (*p & 0x3F);
        p++;
    }
    return (int)cp;
}

static int count_cp(const std::string& s)
{
    const unsigned char* p = (const unsigned char*)s.c_str();
    int n = 0;
    while (*p) { decode_utf8(p); n++; }
    return n;
}

static std::string fit(const std::string& s, int maxChars)
{
    if (count_cp(s) <= maxChars) return s;
    const unsigned char* start = (const unsigned char*)s.c_str();
    const unsigned char* p = start;
    for (int i = 0; i < maxChars - 1; i++) decode_utf8(p);
    return std::string((const char*)start, (size_t)(p - start)) + "\xE2\x80\xA6";
}

// ---------------------------------------------------------------- текст

static void draw_text(const std::string& s, int x, int baseline, Color fg, Color bg)
{
    FT_GlyphSlot slot = g_font->glyph;
    const unsigned char* p = (const unsigned char*)s.c_str();

    while (*p) {
        int cp = decode_utf8(p);
        FT_UInt gi = FT_Get_Char_Index(g_font, (FT_ULong)cp);

        if (FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) continue;
        if (FT_Render_Glyph(slot, ft_render_mode_normal)) continue;

        for (unsigned yy = 0; yy < slot->bitmap.rows; yy++) {
            for (unsigned xx = 0; xx < slot->bitmap.width; xx++) {
                unsigned a = slot->bitmap.buffer[yy * slot->bitmap.pitch + xx];
                if (a == 0) continue;

                int px = x + (int)xx + slot->bitmap_left;
                int py = baseline + (int)yy - slot->bitmap_top;
                if (px < 0 || py < 0 || px >= FRAME_WIDTH || py >= FRAME_HEIGHT) continue;

                Color c;
                c.r = (uint8_t)((bg.r * (255 - a) + fg.r * a) / 255);
                c.g = (uint8_t)((bg.g * (255 - a) + fg.g * a) / 255);
                c.b = (uint8_t)((bg.b * (255 - a) + fg.b * a) / 255);
                g_scene->PutPixel(px, py, c);
            }
        }
        x += (int)(slot->advance.x >> 6);
    }
}

// ---------------------------------------------------------------- иконки кнопок геймпада
// Рисуются кодом (без картинок): Cross, Circle, Square, Triangle, Options (три полоски) и стрелки вверх/вниз.
// Токены в тексте: {cross} {circle} {square} {triangle} {options} {updown}.

enum Icon { ICON_CROSS, ICON_CIRCLE, ICON_SQUARE, ICON_TRIANGLE, ICON_OPTIONS, ICON_UPDOWN };
static const int ICON_S = 20;   // сторона иконки в пикселях - масштабируем вместе с FONT_PX

static bool in_tri(float px, float py, float ax, float ay, float bx, float by, float cx, float cy)
{
    float d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    float d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    float d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
    bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
    return !(neg && pos);
}

// u, v в диапазоне -1..1 (центр иконки в нуле, v растёт вниз).
static bool icon_hit(Icon k, float u, float v)
{
    switch (k) {
    case ICON_CIRCLE: {
        float r = sqrtf(u * u + v * v);
        return r < 0.88f && r > 0.60f;
    }
    case ICON_CROSS: {
        float a = fabsf(u - v) * 0.7071f, b = fabsf(u + v) * 0.7071f;
        return fabsf(u) < 0.82f && fabsf(v) < 0.82f && (a < 0.17f || b < 0.17f);
    }
    case ICON_SQUARE: {
        float m = fmaxf(fabsf(u), fabsf(v));
        return m < 0.80f && m > 0.56f;
    }
    case ICON_TRIANGLE:
        return in_tri(u, v, 0.0f, -0.90f, -0.90f, 0.70f, 0.90f, 0.70f)
            && !in_tri(u, v, 0.0f, -0.42f, -0.495f, 0.46f, 0.495f, 0.46f);
    case ICON_OPTIONS:
        return fabsf(u) < 0.80f && (fabsf(v + 0.55f) < 0.11f || fabsf(v) < 0.11f || fabsf(v - 0.55f) < 0.11f);
    case ICON_UPDOWN:
        return in_tri(u, v, 0.0f, -0.95f, -0.60f, -0.15f, 0.60f, -0.15f)
            || in_tri(u, v, -0.60f, 0.15f, 0.60f, 0.15f, 0.0f, 0.95f);
    }
    return false;
}

static Color icon_color(Icon k)
{
    switch (k) {
    case ICON_CROSS:    { Color c = { 120, 170, 255 }; return c; }   // синий
    case ICON_CIRCLE:   { Color c = { 255, 100, 100 }; return c; }   // красный
    case ICON_SQUARE:   { Color c = { 240, 130, 200 }; return c; }   // розовый
    case ICON_TRIANGLE: { Color c = {  90, 210, 170 }; return c; }   // зелёный
    default:            { Color c = { 200, 204, 212 }; return c; }
    }
}

// top - верхняя граница иконки. Края сглаживаются выборкой 4x4 точки на пиксель.
static void draw_icon(Icon k, int x, int top, Color bg)
{
    Color fg = icon_color(k);
    for (int py = 0; py < ICON_S; py++) {
        for (int px = 0; px < ICON_S; px++) {
            int hit = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    float u = ((px + (sx + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    float v = ((py + (sy + 0.5f) / 4.0f) / ICON_S) * 2.0f - 1.0f;
                    if (icon_hit(k, u, v)) hit++;
                }
            if (hit == 0) continue;
            int xx = x + px, yy = top + py;
            if (xx < 0 || yy < 0 || xx >= FRAME_WIDTH || yy >= FRAME_HEIGHT) continue;
            Color c;
            c.r = (uint8_t)((bg.r * (16 - hit) + fg.r * hit) / 16);
            c.g = (uint8_t)((bg.g * (16 - hit) + fg.g * hit) / 16);
            c.b = (uint8_t)((bg.b * (16 - hit) + fg.b * hit) / 16);
            g_scene->PutPixel(xx, yy, c);
        }
    }
}

struct RichTok { bool isIcon; Icon icon; std::string text; };

static bool icon_by_name(const std::string& n, Icon& out)
{
    if (n == "cross")    { out = ICON_CROSS;    return true; }
    if (n == "circle")   { out = ICON_CIRCLE;   return true; }
    if (n == "square")   { out = ICON_SQUARE;   return true; }
    if (n == "triangle") { out = ICON_TRIANGLE; return true; }
    if (n == "options")  { out = ICON_OPTIONS;  return true; }
    if (n == "updown")   { out = ICON_UPDOWN;   return true; }
    return false;
}

static std::vector<RichTok> rich_parse(const std::string& s)
{
    std::vector<RichTok> out;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            Icon k;
            if (e != std::string::npos && icon_by_name(s.substr(i + 1, e - i - 1), k)) {
                if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); cur.clear(); }
                RichTok t; t.isIcon = true; t.icon = k; out.push_back(t);
                i = e + 1;
                continue;
            }
        }
        cur += s[i++];
    }
    if (!cur.empty()) { RichTok t; t.isIcon = false; t.icon = ICON_CROSS; t.text = cur; out.push_back(t); }
    return out;
}

static int rich_width(const std::vector<RichTok>& toks)
{
    int w = 0;
    for (size_t i = 0; i < toks.size(); i++) w += toks[i].isIcon ? ICON_S : count_cp(toks[i].text) * g_cellW;
    return w;
}

static void put_center_rich(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    std::vector<RichTok> toks = rich_parse(s);
    int x = cx - rich_width(toks) / 2;
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].isIcon) { draw_icon(toks[i].icon, x, baseline - 17, bg); x += ICON_S; }
        else { draw_text(toks[i].text, x, baseline, fg, bg); x += count_cp(toks[i].text) * g_cellW; }
    }
}

static int col_x(int x0, int col) { return x0 + col * g_cellW; }

static void put_left(int x0, int col, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, col_x(x0, col), baseline, fg, bg);
}

static void put_right(int x0, int col, int width, const std::string& s, int baseline, Color fg, Color bg)
{
    std::string t = fit(s, width);
    int n = count_cp(t);
    draw_text(t, col_x(x0, col + width - n), baseline, fg, bg);
}

static void put_center(int cx, const std::string& s, int baseline, Color fg, Color bg)
{
    int w = count_cp(s) * g_cellW;
    draw_text(s, cx - w / 2, baseline, fg, bg);
}

// Рисует картинку (ближайший сосед, с масштабированием) с альфа-смешением по фону bg.
// Фон передаём явно (как и в draw_text), потому что читать обратно из видеопамяти накладно.
static void draw_image(const Image& img, int dstX, int dstY, int dstW, int dstH, Color bg)
{
    if (!img.pixels || img.width <= 0 || img.height <= 0 || dstW <= 0 || dstH <= 0) return;

    for (int y = 0; y < dstH; y++) {
        int sy = y * img.height / dstH;
        int py = dstY + y;
        if (py < 0 || py >= FRAME_HEIGHT) continue;

        for (int x = 0; x < dstW; x++) {
            int sx = x * img.width / dstW;
            int px = dstX + x;
            if (px < 0 || px >= FRAME_WIDTH) continue;

            const unsigned char* s = img.pixels + ((size_t)sy * img.width + sx) * 4;
            unsigned a = s[3];
            if (a == 0) continue;

            Color c;
            c.r = (uint8_t)((bg.r * (255 - a) + s[0] * a) / 255);
            c.g = (uint8_t)((bg.g * (255 - a) + s[1] * a) / 255);
            c.b = (uint8_t)((bg.b * (255 - a) + s[2] * a) / 255);
            g_scene->PutPixel(px, py, c);
        }
    }
}


// ---------------------------------------------------------------- состояние службы

enum SvcState { S_CHECK, S_WAITING, S_READY, S_FAILED, S_STOPPED };
static SvcState    g_svc = S_CHECK;
static std::string g_svcErr;
static std::string g_svcVersion;
static uint64_t    g_waitUntil = 0;
static uint64_t    g_nextPoll = 0;
static int         g_pollFails = 0;
static DisksView   g_disks;
static bool        g_disksOk = false;
static uint64_t    g_nextDisks = 0;
static uint64_t    g_nextSig = 0;
static std::string g_pendingSig;

static std::string version_str() { return APP_VERSION; }
static std::vector<std::string> g_diagLines;

// ---------------------------------------------------------------- навигация: вкладки дисков / смарт+скан / меню
//
// Один экран: сверху горизонтальная строка вкладок (диски + вкладка меню, листаются Left/Right),
// под ней при выбранном (X) диске сразу обе панели - SMART слева и проверка поверхности справа
// (как на веб-странице), а не отдельные экраны. Вниз из строки вкладок - фокус сразу на кнопке
// старта/паузы проверки. На уровне действий Left/Right ходит по кнопкам/полям, Up/Down - назад
// на вкладки. Options (или вкладка меню) открывает вертикальное меню (обновить диски / выключить
// службу): Up/Down - выбор, X - активировать, Circle - закрыть.

enum Focus { FOCUS_TABS, FOCUS_ACTIONS, FOCUS_MENU };
static Focus       g_focus = FOCUS_TABS;
static int         g_tabCursor = 0;   // индекс вкладки: 0..disks.size()-1 - диск, disks.size() - вкладка меню
static std::string g_selId;           // id диска, подтверждённого Cross (его показываем в панелях)
static int         g_actionCursor = 0;
static int         g_menuCursor = 0;

enum Confirm { CONF_NONE, CONF_QUIT, CONF_SCAN_START, CONF_SCAN_STOP };
static Confirm g_confirm = CONF_NONE;

static StateView   g_state;
static uint64_t    g_nextState = 0;
static std::string g_logText;      // хвост /api/log (как на веб-странице), под картой сканирования
static uint64_t    g_nextLog = 0;
static bool        g_scanBusy = false;   // идёт замер скорости (между estimate и подтверждением)
static std::string g_scanMsg;            // временное сообщение экрана сканирования (замер, ошибка)
static EstView      g_confirmEst;        // результат замера, показанный в окне подтверждения старта
static uint64_t     g_rangeFromGb = 0, g_rangeToGb = 0;   // диапазон проверки (0,0 = весь диск)
static uint64_t     g_confirmFromGb = 0, g_confirmToGb = 0; // диапазон, с которым был сделан замер

// Пункты строки действий справа (проверка поверхности): во время работы проверки диапазон
// не редактируется - только пауза/продолжить и стоп.
enum ActKind { AK_FROM, AK_TO, AK_WHOLE, AK_STARTPAUSE, AK_STOP };

static std::vector<ActKind> action_items(bool mineRunning)
{
    std::vector<ActKind> v;
    if (mineRunning) { v.push_back(AK_STARTPAUSE); v.push_back(AK_STOP); }
    else { v.push_back(AK_FROM); v.push_back(AK_TO); v.push_back(AK_WHOLE); v.push_back(AK_STARTPAUSE); }
    return v;
}

static int sel_idx()
{
    for (size_t i = 0; i < g_disks.disks.size(); i++) if (g_disks.disks[i].id == g_selId) return (int)i;
    return -1;
}
static const DiskView* sel_disk()
{
    int i = sel_idx();
    return i < 0 ? NULL : &g_disks.disks[i];
}

// Запуск службы. Один коннект на попытку (без отдельной "пробной" проверки порта перед отправкой -
// два подключения к BinLoader подряд ловили гонку: первое (проверочное) успевало закрыться
// как раз к моменту, когда начиналось второе, и то отваливалось с ECONNREFUSED).
// Если BinLoader GoldHEN (порт 9090) выключен, приложение не может включить его само
// (GoldHEN не перечитывает config.ini на лету) - показываем ошибку с подсказкой,
// Cross повторяет попытку вручную.
static void svc_send_now()
{
    std::string err;
    bool connectFailed = false;
    if (service_send_payload(err, &connectFailed)) {
        g_svc = S_WAITING;
        g_waitUntil = sceKernelGetProcessTime() + 25ULL * 1000000ULL;
        g_nextPoll = sceKernelGetProcessTime() + 1500000ULL;
        g_svcErr.clear();
    } else {
        g_svc = S_FAILED;
        g_svcErr = connectFailed
            ? tr("err_loader_off", "BinLoader is off. Enable it: GoldHEN - Servers settings - Enable Payloader Server")
            : err;
    }
    g_redraw = 2;
}

static void svc_start_payload() { svc_send_now(); }

static void svc_step(uint64_t now)
{
    if (g_svc == S_CHECK) {
        std::string v; bool busy = false;
        if (service_query(v, busy)) {
            g_svcVersion = v;
            if (v == version_str()) {
                logf_("svc: service %s already running", v.c_str());
                g_svc = S_READY; g_pollFails = 0; g_nextDisks = 0; g_nextSig = now + 5000000ULL;
            } else {
                logf_("svc: running service is %s, app is %s: replacing", v.c_str(), version_str().c_str());
                svc_start_payload();
            }
        } else {
            logf_("svc: service not running, starting it");
            svc_start_payload();
        }
        g_redraw = 2;
    } else if (g_svc == S_WAITING) {
        if (now < g_nextPoll) return;
        g_nextPoll = now + 1000000ULL;
        std::string v; bool busy = false;
        if (service_query(v, busy) && v == version_str()) {
            logf_("svc: service is up (%s)", v.c_str());
            g_svcVersion = v;
            g_svc = S_READY; g_pollFails = 0; g_nextDisks = 0; g_nextSig = now + 5000000ULL;
            g_redraw = 2;
        } else if (now > g_waitUntil) {
            logf_("svc: service did not come up in time");
            g_svc = S_FAILED;
            g_svcErr = "no answer from the service";
            g_redraw = 2;
        }
    } else if (g_svc == S_READY) {
        // Диски: читаем список; если службе ещё не велели искать диски, просим.
        if (now >= g_nextDisks) {
            g_nextDisks = now + 3000000ULL;
            DisksView dv;
            if (service_disks(dv)) {
                g_pollFails = 0;
                if (!dv.loaded) service_refresh_disks();
                g_disks = dv; g_disksOk = dv.loaded;
                if (sel_idx() < 0 && !g_disks.disks.empty()) g_selId = g_disks.disks[0].id;
                g_redraw = 2;
            } else if (++g_pollFails >= 3) {
                logf_("svc: service stopped answering, checking again");
                g_svc = S_CHECK; g_disksOk = false; g_redraw = 2;
            }
        }
        // Слежение за сменой дисков (как на веб-странице): подпись совпала дважды подряд, значит диск определился.
        if (g_svc == S_READY && now >= g_nextSig) {
            g_nextSig = now + 5000000ULL;
            std::string sig = service_sig();
            if (!sig.empty() && g_disksOk) {
                if (sig == g_disks.sig) g_pendingSig.clear();
                else if (sig != g_pendingSig) g_pendingSig = sig;
                else { g_pendingSig.clear(); service_refresh_disks(); g_nextDisks = now + 1500000ULL; }
            }
        }
        // Состояние задания/сканирования (для списка, экрана диска и экрана сканирования).
        if (now >= g_nextState) {
            g_nextState = now + 1200000ULL;
            StateView sv;
            if (service_state(sv)) {
                g_state = sv;
                if (g_scanBusy && sv.job == "idle") {
                    g_scanBusy = false;
                    const DiskView* d = sel_disk();
                    if (d && sv.est.present && sv.est.ok && sv.est.disk == d->id) {
                        g_confirmEst = sv.est;
                        g_confirm = CONF_SCAN_START;
                    } else {
                        g_scanMsg = tr("estFail", "Could not measure the disk speed.");
                    }
                }
                g_redraw = 2;
            }
        }
        // Журнал службы (как на веб-странице) - под картой проверки поверхности.
        if (now >= g_nextLog) {
            g_nextLog = now + 2000000ULL;
            std::string t = service_log(4096);
            if (!t.empty()) { g_logText = t; g_redraw = 2; }
        }
    }
}

// ---------------------------------------------------------------- рисование

static std::string fmt_size(uint64_t b)
{
    char t[32];
    double g = (double)b / 1e9;
    if (g >= 1000.0) snprintf(t, sizeof(t), "%.2f TB", g / 1000.0);
    else snprintf(t, sizeof(t), "%.1f GB", g);
    return t;
}

static std::string short_model(const DiskView& d)
{
    std::string m = d.model;
    if (m.empty()) m = d.bridge;
    if (m.empty()) { m = d.id; for (size_t i = 0; i < m.size(); i++) m[i] = (char)toupper((unsigned char)m[i]); return m; }
    size_t dash = m.find('-');
    return dash == std::string::npos ? m : m.substr(0, dash);
}

static std::string fmt_time(double sec)
{
    if (sec < 0) return "—";
    long s = (long)(sec + 0.5);
    long h = s / 3600, m = (s % 3600) / 60, ss = s % 60;
    char b[32];
    if (h) snprintf(b, sizeof(b), "%ldч %ldмин", h, m);
    else if (m) snprintf(b, sizeof(b), "%ldмин %ldс", m, ss);
    else snprintf(b, sizeof(b), "%ldс", ss);
    return b;
}

static std::string fmt_num(uint64_t n)
{
    char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)n);
    std::string s = b, out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) {
        out = s[i] + out;
        if (++c % 3 == 0 && i > 0) out = " " + out;
    }
    return out;
}

// Цвета карты сканирования, как на веб-странице (--b1..--b7). i==0 - ещё не проверено.
static Color hist_color(int i)
{
    switch (i) {
    case 1: { Color c = { 242, 242, 242 }; return c; }
    case 2: { Color c = { 154, 154, 154 }; return c; }
    case 3: { Color c = {  94,  94,  94 }; return c; }
    case 4: { Color c = {  25, 230,  25 }; return c; }
    case 5: { Color c = { 255, 140,   0 }; return c; }
    case 6: { Color c = { 255,  26,  26 }; return c; }
    case 7: { Color c = {  26,  26, 255 }; return c; }
    default:{ Color c = {  42,  46,  54 }; return c; }
    }
}

static void fill(int x, int y, int w, int h, Color c) { g_scene->FillRect(x, y, w, h, c); }

// Скруглённый прямоугольник - полосками по горизонтали (быстро: по пикселю только в углах
// высотой r, остальное - обычные FillRect). Нужен, чтобы вкладки/кнопки/плитки не выглядели
// голыми прямоугольниками, как в этапе 2, а ближе к карточкам веб-интерфейса.
static void fill_rounded(int x, int y, int w, int h, int r, Color c)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { fill(x, y, w, h, c); return; }
    fill(x, y + r, w, h - 2 * r, c);
    for (int yy = 0; yy < r; yy++) {
        double dy = (double)(r - yy) - 0.5;
        double dx2 = (double)r * r - dy * dy;
        int dx = dx2 > 0.0 ? (int)sqrt(dx2) : 0;
        int left = r - dx;
        int rowW = w - 2 * left;
        if (rowW < 0) rowW = 0;
        fill(x + left, y + yy, rowW, 1, c);
        fill(x + left, y + h - 1 - yy, rowW, 1, c);
    }
}

static void fill_circle(int cx, int cy, int r, Color c)
{
    for (int yy = -r; yy <= r; yy++) {
        double dx2 = (double)r * r - (double)yy * yy;
        if (dx2 < 0.0) continue;
        int dx = (int)sqrt(dx2);
        fill(cx - dx, cy + yy, dx * 2 + 1, 1, c);
    }
}

static void draw_panel(int w, int h)
{
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    fill_rounded(x - 3, y - 3, w + 6, h + 6, 18, C_EDGE);
    fill_rounded(x, y, w, h, 16, C_PANEL);
}

static void draw_wrapped(const std::string& s, int x, int yTop, int maxc, int maxLines, Color fg, Color bg)
{
    size_t pos = 0; int line = 0;
    while (pos < s.size() && line < maxLines) {
        size_t end = pos + (size_t)maxc; if (end >= s.size()) end = s.size();
        else { size_t sp = s.rfind(' ', end); if (sp != std::string::npos && sp > pos) end = sp; }
        draw_text(s.substr(pos, end - pos), x, yTop + line * 34, fg, bg);
        pos = end; while (pos < s.size() && s[pos] == ' ') pos++;
        line++;
    }
}

// Последние n непустых строк текста (как хвост терминала) - для панели журнала службы.
static std::vector<std::string> last_lines(const std::string& s, size_t n)
{
    std::vector<std::string> all;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t nl = s.find('\n', pos);
        std::string line = (nl == std::string::npos) ? s.substr(pos) : s.substr(pos, nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) all.push_back(line);
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (all.size() <= n) return all;
    return std::vector<std::string>(all.end() - (long)n, all.end());
}

// ---------------------------------------------------------------- строка вкладок дисков + меню

// x0/w - полоса, оставшаяся справа от логотипа на той же строке (банере), не отдельный ряд.
static void draw_tab_bar(int x0, int y0, int w)
{
    int n = (int)g_disks.disks.size();
    const int menuW = 170, gap = 10;
    int tabsW = w - menuW - gap;
    if (tabsW < 0) tabsW = 0;
    int tabW = n > 0 ? tabsW / n : tabsW;
    int tabH = BANNER_H - 12;
    int ty = y0 + (BANNER_H - tabH) / 2;
    int x = x0;

    for (int i = 0; i < n; i++) {
        const DiskView& d = g_disks.disks[i];
        bool cursorHere = (g_focus == FOCUS_TABS && g_tabCursor == i);
        bool activeHere = (d.id == g_selId);
        Color bg = activeHere ? C_PANEL : C_ROW2;
        if (cursorHere) {
            fill_rounded(x, ty, tabW - gap, tabH, 10, C_EDGE);
            fill_rounded(x + 3, ty + 3, tabW - gap - 6, tabH - 6, 8, bg);
        } else {
            fill_rounded(x, ty, tabW - gap, tabH, 10, bg);
        }
        std::string label = fit(short_model(d) + " " + d.id, (tabW - 48) / g_cellW);
        draw_text(label, x + 16, ty + tabH / 2 + 8, activeHere ? C_TEXT : C_DIM, bg);

        Color dot = C_DIM;
        if (d.hasSmart && d.verdict == 0) dot = C_GREEN;
        else if (d.hasSmart && d.verdict == 1) dot = C_AMBER;
        else if (d.hasSmart && d.verdict == 2) dot = C_RED;
        if (g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d.id) dot = C_AMBER;
        fill_circle(x + tabW - gap - 18, ty + tabH / 2, 7, dot);

        x += tabW;
    }

    bool menuCursorHere = (g_focus == FOCUS_TABS && g_tabCursor == n) || g_focus == FOCUS_MENU;
    Color mbg = menuCursorHere ? C_PANEL : C_ROW2;
    int mx = x0 + tabsW + gap;
    if (menuCursorHere) {
        fill_rounded(mx, ty, menuW, tabH, 10, C_EDGE);
        fill_rounded(mx + 3, ty + 3, menuW - 6, tabH - 6, 8, mbg);
    } else {
        fill_rounded(mx, ty, menuW, tabH, 10, mbg);
    }
    std::string mlabel = "{options} " + tr("menu_title", "Menu");
    put_center_rich(mx + menuW / 2, mlabel, ty + tabH / 2 + 8, C_TEXT, mbg);
}

// ---------------------------------------------------------------- экран: SMART диска

static std::string reason_text(const DiskView& d, const SmartReasonView& r)
{
    std::string name;
    for (size_t k = 0; k < d.attrs.size(); k++) if (d.attrs[k].id == r.id) { name = d.attrs[k].name; break; }
    char v[32]; snprintf(v, sizeof(v), "%lld", (long long)r.value);
    if (r.code == "status_fail") return tr("r_status_fail", "The drive reports imminent failure (SMART RETURN STATUS). Back up your data now.");
    if (r.code == "temp") return tr("r_temp_prefix", "High temperature:") + std::string(" ") + v + " °C";
    if (r.code == "thr") return name + " " + tr("r_thr_suffix", "dropped to or below threshold") + " (" + v + ")";
    return name + ": " + v;
}

// x0/w - левая панель (SMART), уже не вся ширина экрана, как раньше, а ~44% (справа - проверка поверхности).
static void draw_screen_disk(const DiskView& d, int x0, int w, int y0)
{
    int charsAvail = w / g_cellW;
    std::string header = (!d.model.empty() ? d.model : !d.bridge.empty() ? d.bridge : d.id) + " · " + fmt_size(d.bytes);
    draw_text(fit(header, charsAvail), x0, y0 + 30, C_TEXT, C_BG);

    if (!d.hasSmart) {
        draw_wrapped(tr("noSmartHint", "This disk does not provide SMART (common with USB enclosures). Surface scan is available."),
                     x0, y0 + 80, charsAvail - 2, 3, C_DIM, C_BG);
        draw_text(fit(tr("path", "Device") + ": " + d.path, charsAvail), x0, y0 + 190, C_DIM, C_BG);
        return;
    }

    int y1 = y0 + 76;
    std::string vt; Color vc = C_DIM;
    if (d.verdict == 0) { vt = tr("v0", "Healthy"); vc = C_GREEN; }
    else if (d.verdict == 1) { vt = tr("v1", "Caution"); vc = C_AMBER; }
    else if (d.verdict == 2) { vt = tr("v2", "Bad"); vc = C_RED; }
    else vt = tr("vn", "No SMART");
    draw_text(vt, x0, y1, vc, C_BG);
    if (d.temp > 0) { char t[32]; snprintf(t, sizeof(t), "%d °C", d.temp); draw_text(t, x0 + 220, y1, C_DIM, C_BG); }

    struct KV { std::string k, v; };
    std::vector<KV> kv;
    kv.push_back(KV{ tr("fw", "Firmware"), d.firmware.empty() ? "—" : d.firmware });
    kv.push_back(KV{ tr("serial", "Serial number"), d.serial.empty() ? "—" : d.serial });
    kv.push_back(KV{ tr("iface", "Interface"), d.proto + (d.bridge.empty() ? "" : (" · " + d.bridge)) });
    kv.push_back(KV{ tr("rot", "Rotation rate"), d.rpm == 1 ? tr("ssd", "SSD") : d.rpm > 1 ? (fmt_num((uint64_t)d.rpm) + " " + tr("rpm", "rpm")) : "—" });
    kv.push_back(KV{ tr("hr", "Total host reads"), d.hostRead >= 0 ? fmt_size((uint64_t)d.hostRead) : "—" });
    kv.push_back(KV{ tr("hw", "Total host writes"), d.hostWrite >= 0 ? fmt_size((uint64_t)d.hostWrite) : "—" });
    kv.push_back(KV{ tr("cycles", "Power cycles"), d.cycles >= 0 ? fmt_num((uint64_t)d.cycles) : "—" });
    kv.push_back(KV{ tr("poh", "Power-on hours"), d.poh >= 0 ? fmt_num((uint64_t)d.poh) : "—" });

    int gy = y1 + 42;
    int valX = x0 + 260;
    int valChars = (w - 260) / g_cellW; if (valChars < 4) valChars = 4;
    for (size_t i = 0; i < kv.size(); i++) {
        draw_text(fit(kv[i].k + ":", 30), x0, gy + (int)i * 34, C_DIM, C_BG);
        draw_text(fit(kv[i].v, valChars), valX, gy + (int)i * 34, C_TEXT, C_BG);
    }

    int y2 = gy + (int)kv.size() * 34 + 18;
    if (d.reasons.empty() && d.verdict == 0) {
        draw_text(tr("ok", "No critical indicators found."), x0, y2, C_GREEN, C_BG);
        y2 += 32;
    } else {
        size_t shown = 0;
        for (size_t i = 0; i < d.reasons.size() && shown < 3; i++, shown++) {
            Color rc = d.reasons[i].sev == 2 ? C_RED : C_AMBER;
            draw_wrapped(reason_text(d, d.reasons[i]), x0, y2, charsAvail - 2, 2, rc, C_BG);
            y2 += 62;
        }
    }

    y2 += 10;
    draw_text(tr("attrs", "SMART attributes"), x0, y2, C_TEXT, C_BG);
    y2 += 30;
    // Колонки считаем от реальной доступной ширины панели (charsAvail), а не фиксированными
    // символами под старую, узкую панель - иначе таблица занимает только часть панели, а
    // оставшееся место выглядит как пустой провал перед блоком проверки.
    int cId = 0, cName = 4;
    int cRaw = charsAvail - 13, cThr = charsAvail - 21, cWorst = charsAvail - 29, cCur = charsAvail - 37;
    int nameW = cCur - cName - 2; if (nameW < 10) nameW = 10;
    put_left(x0, cId, tr("id", "ID"), y2, C_DIM, C_BG);
    put_left(x0, cName, tr("name", "Name"), y2, C_DIM, C_BG);
    put_right(x0, cCur, 6, tr("cur", "Current"), y2, C_DIM, C_BG);
    put_right(x0, cWorst, 6, tr("worst", "Worst"), y2, C_DIM, C_BG);
    put_right(x0, cThr, 6, tr("thr", "Thresh"), y2, C_DIM, C_BG);
    put_right(x0, cRaw, 12, tr("raw", "Raw"), y2, C_DIM, C_BG);
    y2 += 26;
    for (size_t i = 0; i < d.attrs.size(); i++) {
        if (y2 > 1000) break;
        const SmartAttrView& a = d.attrs[i];
        Color rc = a.sev == 2 ? C_RED : a.sev == 1 ? C_AMBER : C_TEXT;
        char idhex[8]; snprintf(idhex, sizeof(idhex), "%02X", (unsigned)a.id);
        put_left(x0, cId, idhex, y2, rc, C_BG);
        put_left(x0, cName, fit(a.name, nameW), y2, C_TEXT, C_BG);
        put_right(x0, cCur, 6, fmt_num((uint64_t)a.cur), y2, C_TEXT, C_BG);
        put_right(x0, cWorst, 6, fmt_num((uint64_t)a.worst), y2, C_TEXT, C_BG);
        put_right(x0, cThr, 6, fmt_num((uint64_t)a.thresh), y2, C_TEXT, C_BG);
        put_right(x0, cRaw, 12, fit(a.raw, 12), y2, C_DIMMER, C_BG);
        y2 += 26;
    }
}

// ---------------------------------------------------------------- экран: проверка поверхности

// x0/w - правая панель (проверка поверхности), ~56% ширины экрана.
static void draw_screen_scan(const DiskView& d, int x0, int w, int y0, const std::vector<ActKind>& items, bool mineRunning)
{
    bool mine = g_state.scan.present && g_state.scan.disk == d.id;
    const ScanView& sc = g_state.scan;
    const std::string& job = g_state.job;

    std::string header = tr("scanTitle", "Surface scan") + " · " + short_model(d) + " (" + d.id + ")";
    draw_text(fit(header, w / g_cellW), x0, y0 + 30, C_TEXT, C_BG);

    // Строка действий: диапазон проверки (From/To, системная клавиатура) и старт/пауза/стоп.
    int ay = y0 + 54, ah = 40;
    int n = (int)items.size();
    int agap = 8;
    int itemW = n > 0 ? (w - (n - 1) * agap) / n : w;
    int ax = x0;
    for (int i = 0; i < n; i++) {
        bool cur = (g_focus == FOCUS_ACTIONS && g_actionCursor == i);
        Color bg = cur ? C_EDGE : C_PANEL;
        fill_rounded(ax, ay, itemW, ah, 10, bg);
        std::string label;
        switch (items[i]) {
        case AK_FROM: {
            char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)g_rangeFromGb);
            label = tr("act_from", "From") + ": " + b + " GB";
            break;
        }
        case AK_TO: {
            if (g_rangeToGb == 0) label = tr("act_to", "To") + ": " + tr("act_end", "end");
            else { char b[32]; snprintf(b, sizeof(b), "%llu", (unsigned long long)g_rangeToGb); label = tr("act_to", "To") + ": " + b + " GB"; }
            break;
        }
        case AK_WHOLE: label = tr("act_whole", "Whole disk"); break;
        case AK_STARTPAUSE:
            if (mineRunning) label = (sc.state == "paused") ? tr("hint_resume", "resume") : tr("hint_pause", "pause");
            else label = tr("hint_start", "start scan");
            break;
        case AK_STOP: label = tr("hint_stop", "stop"); break;
        }
        put_center(ax + itemW / 2, fit(label, (itemW - 16) / g_cellW), ay + ah - 14, C_TEXT, bg);
        ax += itemW + agap;
    }

    int msgY = ay + ah + 30;
    std::string msg;
    if (g_scanBusy) msg = tr("bench", "Measuring read speed…");
    else if (mine && job == "scan") msg = sc.state == "paused" ? tr("paused", "Paused") : sc.state == "stopping" ? tr("stopping", "Stopping…") : tr("running", "Scanning the surface…");
    else if (mine && !sc.msg.empty() && sc.state != "running") msg = tr(("m_" + sc.msg).c_str(), sc.msg.c_str());
    else if (!g_scanMsg.empty()) msg = g_scanMsg;
    else msg = tr("idle", "Ready. The scan only reads data and never writes.");
    draw_text(fit(msg, w / g_cellW), x0, msgY, C_DIM, C_BG);

    double pct = 0;
    if (mine) { uint64_t tot = sc.to - sc.from; pct = tot > 0 ? 100.0 * (double)sc.done / (double)tot : 0; }
    int barY = msgY + 24, barW = w, barH = 12;
    fill(x0, barY, barW, barH, Color{ 30, 33, 40 });
    fill(x0, barY, (int)((double)barW * pct / 100.0), barH, Color{ 91, 157, 255 });

    // Легенда занимала почти 40% ширины - сузили почти вдвое (~18%), карта получила освободившееся
    // место и стала крупнее (по просьбе пользователя).
    const int cols = 25, rows = 20, gap = 2, cellH = 16;
    int legGap = 16, legW = (int)(w * 0.23);   // чуть шире - подписи категорий меньше обрезались
    int mapW = w - legW - legGap;
    int cellW = (mapW - (cols - 1) * gap) / cols;
    int mapY = barY + 26;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            int v = (mine && idx < (int)sc.map.size()) ? (sc.map[(size_t)idx] - '0') : 0;
            fill(x0 + c * (cellW + gap), mapY + r * (cellH + gap), cellW, cellH, hist_color(v));
        }
    }
    int mapH = rows * (cellH + gap) - gap;

    int legX = x0 + mapW + legGap;
    int legRowH = mapH / 7;
    for (int i = 1; i <= 7; i++) {
        int ly = mapY + (i - 1) * legRowH;
        fill_rounded(legX, ly, legW, legRowH - 4, 6, C_PANEL);
        fill_rounded(legX + 6, ly + (legRowH - 4) / 2 - 7, 14, 14, 3, hist_color(i));
        char key[4]; snprintf(key, sizeof(key), "h%d", i);
        draw_text(fit(tr(key, ""), (legW - 26) / g_cellW), legX + 24, ly + (legRowH - 4) / 2 + 7, C_TEXT, C_PANEL);
    }

    int statY = mapY + mapH + 20;
    struct St { std::string k, v; };
    std::vector<St> stats;
    { char b[32]; snprintf(b, sizeof(b), "%.1f %%", pct); stats.push_back(St{ tr("prog", "Progress"), b }); }
    stats.push_back(St{ tr("speed", "Speed"), (mine && sc.speed > 0) ? (fmt_num((uint64_t)(sc.speed / 1e6)) + " MB/s") : "—" });
    stats.push_back(St{ tr("elapsed", "Elapsed"), mine ? fmt_time(sc.elapsed) : "—" });
    stats.push_back(St{ tr("left", "Remaining"), (mine && job == "scan" && sc.eta >= 0) ? fmt_time((double)sc.eta) : "—" });
    stats.push_back(St{ tr("pos", "Position"), mine ? fmt_size(sc.from + sc.done) : "—" });
    stats.push_back(St{ tr("bad", "Bad sectors"), mine ? (fmt_num(sc.bad) + (sc.approx ? " *" : "")) : "—" });
    int sw = w / 3;
    for (size_t i = 0; i < stats.size(); i++) {
        int cx = x0 + (int)(i % 3) * sw, cy = statY + (int)(i / 3) * 56;
        fill_rounded(cx, cy, sw - 10, 48, 10, C_PANEL);
        draw_text(fit(stats[i].k, (sw - 24) / g_cellW), cx + 12, cy + 20, C_DIM, C_PANEL);
        draw_text(fit(stats[i].v, (sw - 24) / g_cellW), cx + 12, cy + 42, C_TEXT, C_PANEL);
    }

    // Журнал службы (как на веб-странице) - под плитками статистики, до низа панели.
    int logY = statY + 2 * 56 + 16;
    int logH = 996 - logY;
    if (logH > 40) {
        fill_rounded(x0, logY, w, logH, 10, C_PANEL);
        draw_text(tr("log", "Log"), x0 + 14, logY + 24, C_DIM, C_PANEL);
        int lineH = 22;
        int maxLines = (logH - 34) / lineH; if (maxLines < 0) maxLines = 0;
        std::vector<std::string> lines = last_lines(g_logText, (size_t)maxLines);
        int chars = (w - 28) / g_cellW;
        int ly = logY + 24 + lineH;
        for (size_t i = 0; i < lines.size(); i++) {
            draw_text(fit(lines[i], chars), x0 + 14, ly, C_DIMMER, C_PANEL);
            ly += lineH;
        }
    }
}

static const int MENU_ITEMS = 4;   // обновить диски / язык / донат / выключить службу

static void draw_menu_overlay()
{
    int w = 460, h = 60 + MENU_ITEMS * 56;
    draw_panel(w, h);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    for (int i = 0; i < MENU_ITEMS; i++) {
        int ry = y + 30 + i * 56;
        bool cur = (g_menuCursor == i);
        if (cur) fill_rounded(x + 20, ry - 8, w - 40, 44, 10, C_EDGE);
        std::string label;
        switch (i) {
        case 0: label = tr("menu_refresh", "Refresh disks"); break;
        case 1: label = tr("menu_lang", "Language") + ": " + (i18n_current() == "ru" ? "Русский" : "English"); break;
        case 2: label = tr("menu_donate", "Support the project"); break;
        case 3: label = tr("menu_quit", "Shut down the service"); break;
        }
        draw_text(label, x + 40, ry + 22, C_TEXT, cur ? C_EDGE : C_PANEL);
    }
    std::string hint = "{updown} " + tr("hint_select", "select") + "     {cross} " + tr("hint_open", "activate") + "     {circle} " + tr("hint_back", "back");
    put_center_rich(x + w / 2, hint, y + h - 16, C_DIM, C_PANEL);
}

// QR для донатов - СБП для русского языка, PayPal иначе (как на веб-странице). Грузится лениво,
// один раз при первом открытии (чтобы не читать PNG, если донат никогда не открывают).
static void draw_donate_overlay()
{
    if (!g_qrTried) {
        g_qrTried = true;
        g_qrSbpOk = g_qrSbp.load("/app0/assets/images/qr_sbp.png");
        g_qrPaypalOk = g_qrPaypal.load("/app0/assets/images/qr_paypal.png");
    }
    bool ru = (i18n_current() == "ru");
    const Image& qr = ru ? g_qrSbp : g_qrPaypal;
    bool ok = ru ? g_qrSbpOk : g_qrPaypalOk;

    int w = 420, h = 480;
    draw_panel(w, h);
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    put_center(x + w / 2, tr("menu_donate", "Support the project"), y + 46, C_TEXT, C_PANEL);
    if (ok) {
        int qs = 320;
        draw_image(qr, x + (w - qs) / 2, y + 70, qs, qs, C_PANEL);
    } else {
        draw_text(tr("donate_missing", "QR code not found"), x + 40, y + h / 2, C_DIM, C_PANEL);
    }
    std::string hint = "{circle} " + tr("hint_back", "back");
    put_center_rich(x + w / 2, hint, y + h - 24, C_DIM, C_PANEL);
}

static void draw_all(uint64_t now)
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);

    // Логотип и строка вкладок - на одной строке (раньше вкладки были отдельным рядом ниже,
    // из-за чего всё выглядело непропорционально просевшим). Статус службы теперь не текст
    // ("Служба работает" + версия - версия уже есть внизу, в подписи), а один кружок в углу:
    // зелёный - работает, красный - нет.
    int logoW;
    if (g_bannerOk) { draw_image(g_banner, BANNER_LEFT, BANNER_TOP, g_bannerW, BANNER_H, C_BG); logoW = g_bannerW; }
    else { draw_text("PS4 HDD TOOLS", MARGIN_X, BANNER_TOP + 44, C_TEXT, C_BG); logoW = count_cp("PS4 HDD TOOLS") * g_cellW; }

    const int dotReserve = 46;
    int barX0 = BANNER_LEFT + logoW + 36;
    int barW = FRAME_WIDTH - MARGIN_X - dotReserve - barX0;

    Color dot = (g_svc == S_READY) ? C_GREEN : C_RED;
    fill_circle(FRAME_WIDTH - MARGIN_X - 12, BANNER_TOP + BANNER_H / 2, 11, dot);

    int y0 = BANNER_TOP + BANNER_H + 20;
    std::string hint;

    if (g_svc == S_FAILED) {
        draw_text(g_svcErr, MARGIN_X, y0 + 30, C_RED, C_BG);
        draw_text(tr("svc_hint_fail", "Make sure GoldHEN is running (BinLoader, port 9090). Cross - try again"), MARGIN_X, y0 + 80, C_DIM, C_BG);
    } else if (g_svc == S_STOPPED) {
        draw_text(tr("svc_hint_stopped", "Cross - start the service again"), MARGIN_X, y0 + 30, C_DIM, C_BG);
    } else if (g_svc == S_CHECK || g_svc == S_WAITING) {
        draw_text(g_svc == S_CHECK ? tr("svc_check", "Looking for the service…") : tr("svc_waiting", "Waiting for the service to start…"), MARGIN_X, y0 + 30, C_DIM, C_BG);
    } else if (g_svc == S_READY) {
        draw_tab_bar(barX0, BANNER_TOP, barW);
        int bodyY = y0;
        const DiskView* d = sel_disk();

        if (!g_disksOk) {
            draw_text(tr("loading_disks", "Reading disks…"), MARGIN_X, bodyY + 30, C_DIM, C_BG);
        } else if (g_disks.disks.empty()) {
            draw_text(tr("no_disks", "No disks found"), MARGIN_X, bodyY + 30, C_DIM, C_BG);
        } else if (!d) {
            draw_text(tr("pick_disk", "Select a disk above and press Cross"), MARGIN_X, bodyY + 30, C_DIM, C_BG);
        } else {
            const int leftW = 880, gapX = 14;   // SMART чуть уже - деление экрана выглядело смещённым не по центру, правая панель (проверка) забирает освободившееся
            int rightX = MARGIN_X + leftW + gapX;
            int rightW = FRAME_WIDTH - rightX - MARGIN_X;
            bool mineRunning = g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d->id;
            std::vector<ActKind> items = action_items(mineRunning);
            draw_screen_disk(*d, MARGIN_X, leftW, bodyY);
            draw_screen_scan(*d, rightX, rightW, bodyY, items, mineRunning);
        }

        if (g_focus == FOCUS_MENU || g_showDonate) {
            hint.clear();
        } else if (g_focus == FOCUS_ACTIONS) {
            hint = "{<>} " + tr("hint_switch", "switch") + "     {cross} " + tr("hint_edit", "edit") + "     {updown} " + tr("hint_back", "back");
        } else if (!g_disks.disks.empty()) {
            hint = "{<>} " + tr("hint_switch", "switch") + "     {cross} " + tr("hint_open", "select disk");
            if (d) hint += "     {updown} " + tr("hint_scan", "surface scan");
            hint += "     {square} " + tr("hint_refresh", "refresh disks") + "     {options} " + tr("hint_menu", "menu");
        } else {
            hint = "{square} " + tr("hint_refresh", "refresh disks") + "     {options} " + tr("hint_menu", "menu");
        }
    }

    for (size_t i = 0; i < g_diagLines.size(); i++) draw_text(g_diagLines[i].substr(0, 130), MARGIN_X, 900 + (int)i * 30, C_DIMMER, C_BG);

    if (!hint.empty() && g_confirm == CONF_NONE) put_center_rich(FRAME_WIDTH / 2, hint, 1010, C_DIM, C_BG);
    std::string credit = "PS4 HDD Tools v" + version_str() + " Created by SergioPoverony and Mr.Claude";
    draw_text(credit, (FRAME_WIDTH - count_cp(credit) * g_cellW) / 2, 1050, C_DIMMER, C_BG);

    if (g_showDonate && g_confirm == CONF_NONE) draw_donate_overlay();
    else if (g_focus == FOCUS_MENU && g_confirm == CONF_NONE) draw_menu_overlay();

    if (g_confirm != CONF_NONE) {
        draw_panel(1040, 340);
        int cx = FRAME_WIDTH / 2, y = (FRAME_HEIGHT - 340) / 2;
        std::string title, body, yesLbl;
        if (g_confirm == CONF_QUIT) {
            title = tr("quit_title", "Shut down the service?");
            body = tr("quit_desc", "");
            yesLbl = tr("yes", "shut down");
        } else if (g_confirm == CONF_SCAN_START) {
            const DiskView* d = sel_disk();
            title = tr("scan_start_q", "Start scanning disk") + " " + (d ? d->id : "") + "?";
            char b[256];
            snprintf(b, sizeof(b), "%.1f GB, ~%s, ~%.0f MB/s. %s",
                     (double)(g_confirmEst.to - g_confirmEst.from) / 1e9,
                     fmt_time((double)g_confirmEst.seconds).c_str(),
                     g_confirmEst.speed / 1e6,
                     tr("cWarn", "The disk and the console will be heavily loaded. No data is modified.").c_str());
            body = b;
            yesLbl = tr("cYes", "Start scan");
        } else if (g_confirm == CONF_SCAN_STOP) {
            title = tr("stopQ", "Stop the scan?");
            yesLbl = tr("sYes", "Stop");
        }
        put_center(cx, title, y + 60, C_TEXT, C_PANEL);
        draw_wrapped(body, cx - 480, y + 110, 62, 4, C_DIM, C_PANEL);
        std::string b2 = "{cross} " + yesLbl + "     {circle} " + tr("cancel", "cancel");
        put_center_rich(cx, b2, y + 300, C_TEXT, C_PANEL);
    }
}

// ---------------------------------------------------------------- геймпад

static int  g_pad = -1;
static int  g_userId = 0;
static bool g_imeReady = false;    // системные модули клавиатуры загружены (sceSysmoduleLoadModule)

static bool pad_init()
{
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int userID = 0;
    sceUserServiceGetInitialUser(&userID);
    g_userId = userID;
    g_pad = scePadOpen(userID, 0, 0, NULL);
    return g_pad >= 0;
}

static unsigned pad_read()
{
    if (g_pad < 0) return 0;
    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    scePadReadState(g_pad, &data);
    return (unsigned)data.buttons;
}

// Системная клавиатура PS4 (IME Dialog) для полей "от"/"до" в ГБ: предзаполняем текущим значением,
// после подтверждения (ORBIS_DIALOG_OK) разбираем введённое число обратно в out.
//
// ВАЖНО (нашлось при проверке - клавиатура открывалась, но с пустым полем и введённое не
// передавалось обратно): поле inputTextBuffer в заголовках объявлено как wchar_t*, но на этой
// платформе (FreeBSD/PS4 ABI) sizeof(wchar_t)==4, а системная клавиатура работает с UTF-16
// (2 байта на символ) - так же, как это обходит реальное рабочее PS4-приложение с таким же
// диалогом (bucanero/apollo-ps4: держит буфер как uint16_t[], а в структуру отдаёт его же,
// приведённый к (wchar_t*)). Раньше здесь был честный wchar_t[32] с swprintf/swscanf (4 байта
// на символ) - система писала/читала его как массив UTF-16 (2 байта), из-за несовпадения
// размера элемента буфер эффективно читался/писался невпопад: отсюда пустое поле при открытии
// и то, что введённое не доходило до out. Сейчас буфер - uint16_t[], заполняется и разбирается
// вручную (цифры ASCII совпадают с их кодами UTF-16, поэтому конвертировать можно напрямую).
// Переводит UTF-8 строку (заголовки берутся из lang_*.json, там кириллица) в UTF-16 код-юниты
// для системной клавиатуры. Переиспользует decode_utf8() (см. выше) вместо побайтового
// копирования - тот способ ломал кириллицу ("код как будто испорчен"), так как один символ
// кириллицы в UTF-8 - это 2 байта, а не один UTF-16 код-юнит. Кодовые точки вне BMP (выше
// 0xFFFF - нам не встречаются) кодируются суррогатной парой на случай будущих языков.
static size_t utf8_to_utf16(const std::string& s, uint16_t* out, size_t outCap)
{
    size_t n = 0;
    const unsigned char* p = (const unsigned char*)s.data();
    const unsigned char* end = p + s.size();
    while (p < end && n + 1 < outCap) {
        int cp = decode_utf8(p);
        if (cp < 0) break;
        if (cp <= 0xFFFF) {
            out[n++] = (uint16_t)cp;
        } else if (n + 2 < outCap) {
            cp -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (cp >> 10));
            out[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            break;
        }
    }
    out[n] = 0;
    return n;
}

static bool keyboard_edit_number(const std::string& title, uint64_t current, uint64_t maxVal, uint64_t& out)
{
    if (!g_imeReady) { logf_("ui: ime dialog not ready, skip keyboard"); return false; }

    uint16_t buf[32];
    uint16_t wtitle[64];
    memset(buf, 0, sizeof(buf));
    memset(wtitle, 0, sizeof(wtitle));

    // Если текущее значение не задано (0 = "от начала"/"до конца диска", не настоящее число),
    // не подставляем "0" в поле - пользователь просил: "лучше ничего не отображать", чем
    // значение, которое выглядит как введённое, а на деле просто плейсхолдер диапазона.
    if (current != 0) {
        char digits[32];
        snprintf(digits, sizeof(digits), "%llu", (unsigned long long)current);
        size_t dn = strlen(digits);
        if (dn > 31) dn = 31;
        for (size_t i = 0; i < dn; i++) buf[i] = (uint16_t)(unsigned char)digits[i];
    }

    utf8_to_utf16(title, wtitle, 64);

    OrbisImeDialogSetting setting;
    memset(&setting, 0, sizeof(setting));
    setting.userId = (uint32_t)g_userId;
    // Раньше здесь стоял ORBIS_TYPE_DEFAULT - подстраховка на случай, если дело было в типе
    // диалога (оказалось - не в нём, а в испорченном заголовке из-за кириллицы, см. выше
    // utf8_to_utf16()); DEFAULT показывал буквенную клавиатуру вместо цифровой, что неудобно
    // для ввода ГБ. Возвращён ORBIS_TYPE_NUMBER - сразу цифровая клавиатура.
    setting.type = ORBIS_TYPE_NUMBER;
    setting.supportedLanguages = 0;
    setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    setting.inputMethod = ORBIS__DEFAULT;
    setting.filter = NULL;
    setting.option = 0;
    setting.maxTextLength = 10;
    setting.inputTextBuffer = (wchar_t*)buf;      // поле типа wchar_t*, но реально UTF-16 (см. выше)
    setting.posx = (float)(FRAME_WIDTH / 2);
    setting.posy = (float)(FRAME_HEIGHT / 2);
    setting.horizontalAlignment = ORBIS_H_CENTER;
    setting.verticalAlignment = ORBIS_V_CENTER;
    setting.placeholder = NULL;
    setting.title = (const wchar_t*)wtitle;

    if (sceImeDialogInit(&setting, NULL) != 0) { logf_("ui: sceImeDialogInit failed"); return false; }

    OrbisDialogStatus st;
    while ((st = sceImeDialogGetStatus()) == ORBIS_DIALOG_STATUS_RUNNING) {
        // Поддерживаем конвейер вывода живым, пока системный диалог открыт поверх нашего кадра.
        if (g_scene) {
            g_scene->SubmitFlip(g_frameID);
            g_scene->FrameWait(g_frameID);
            g_scene->FrameBufferSwap();
            g_frameID++;
        }
        usleep(16000);
    }

    bool ok = false;
    if (st == ORBIS_DIALOG_STATUS_STOPPED) {
        OrbisDialogResult res;
        memset(&res, 0, sizeof(res));
        if (sceImeDialogGetResult(&res) == 0 && res.endstatus == ORBIS_DIALOG_OK) {
            uint64_t v = 0;
            bool any = false;
            for (int i = 0; i < 32 && buf[i] != 0; i++) {
                uint16_t c = buf[i];
                if (c >= '0' && c <= '9') { v = v * 10 + (uint64_t)(c - '0'); any = true; }
            }
            if (any) {
                if (v > maxVal) v = maxVal;
                out = v;
                ok = true;
            }
        }
    }
    sceImeDialogTerm();
    g_redraw = 2;
    return ok;
}

bool ui_init()
{
    g_scene = new Scene2D(FRAME_WIDTH, FRAME_HEIGHT, FRAME_DEPTH);
    if (!g_scene->Init(0xC000000, 2)) { logf_("ui: scene init FAILED"); return false; }
    if (!g_scene->InitFont(&g_font, "/app0/assets/fonts/DejaVuSansMono.ttf", FONT_PX)) { logf_("ui: font init FAILED"); return false; }

    FT_UInt gi = FT_Get_Char_Index(g_font, 'M');
    if (!FT_Load_Glyph(g_font, gi, FT_LOAD_DEFAULT)) {
        int w = (int)(g_font->glyph->advance.x >> 6);
        if (w > 0) g_cellW = w;
    }
    g_bannerOk = g_banner.load("/app0/assets/images/title.png");
    if (g_bannerOk) g_bannerW = BANNER_H * g_banner.width / g_banner.height;

    // Опыт с GoldHEN SDK (syscall 500) завершён: на консоли вызов не проходит из песочницы
    // обычного приложения (ok=0), путь закрыт - см. HANDOFF.md. Диагностику убрали с экрана.
    bool padOk = pad_init();

    // Системная клавиатура (From/To, ГБ): грузим её системные модули заранее, чтобы при первом
    // открытии не было задержки/сбоя. Не критично для остального интерфейса, если не загрузится -
    // просто кнопки полей From/To по Cross ничего не сделают (см. keyboard_edit_number).
    //
    // ВАЖНО (нашёл после сверки с рабочим примером - apollo-ps4, реальное PS4-приложение с таким
    // же диалогом): ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG - это "внутренний" модуль, его грузит
    // ОТДЕЛЬНАЯ функция sceSysmoduleLoadModuleInternal(), а не обычный sceSysmoduleLoadModule().
    // Раньше здесь был вызов обычной функции с приведением типа - CommonDialog из-за этого не
    // инициализировался, и именно поэтому клавиатура вообще не открывалась (sceImeDialogInit
    // не может показать диалог без исправно поднятого CommonDialog).
    bool imeMod = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG) == 0;
    bool cdMod  = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_COMMON_DIALOG) == 0;
    if (cdMod) cdMod = sceCommonDialogInitialize() == 0;
    g_imeReady = imeMod && cdMod;
    logf_("ui: ime dialog modules ime=%d commonDialog=%d", imeMod ? 1 : 0, cdMod ? 1 : 0);

    logf_("ui: ready, cell width %d, pad %s", g_cellW, padOk ? "ok" : "FAILED");
    g_ready = true;
    return true;
}

void ui_pump()
{
    if (!g_ready) return;
    uint64_t now = sceKernelGetProcessTime();

    svc_step(now);

    unsigned b = pad_read();
    static unsigned prev = 0;
    unsigned pressed = b & ~prev;
    prev = b;
    bool cross    = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
    bool circle   = (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0;
    bool square   = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    bool options  = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    bool up       = (pressed & ORBIS_PAD_BUTTON_UP) != 0;
    bool down     = (pressed & ORBIS_PAD_BUTTON_DOWN) != 0;
    bool left     = (pressed & ORBIS_PAD_BUTTON_LEFT) != 0;
    bool right    = (pressed & ORBIS_PAD_BUTTON_RIGHT) != 0;

    if (g_showDonate) {
        if (circle || cross) { g_showDonate = false; g_redraw = 2; }
    } else if (g_confirm != CONF_NONE) {
        if (cross) {
            if (g_confirm == CONF_QUIT) {
                service_quit();
                g_svc = S_STOPPED; g_disksOk = false; g_selId.clear(); g_focus = FOCUS_TABS;
            } else if (g_confirm == CONF_SCAN_START) {
                const DiskView* d = sel_disk();
                if (d) {
                    service_scan_start(d->id, g_confirmFromGb, g_confirmToGb);
                    // Карта предыдущей проверки (если была) иначе видна ещё секунду-две, пока не
                    // придёт первый свежий /api/state - выглядит как "не обнулилось". Чистим сразу,
                    // не дожидаясь опроса.
                    g_state.scan.map.clear();
                    for (int i = 0; i < 8; i++) g_state.scan.hist[i] = 0;
                    g_state.scan.done = 0; g_state.scan.bad = 0;
                    g_nextState = now;
                }
            } else if (g_confirm == CONF_SCAN_STOP) {
                service_scan_stop(); g_nextState = now;
            }
            g_confirm = CONF_NONE; g_redraw = 2;
        } else if (circle) { g_confirm = CONF_NONE; g_redraw = 2; }
    } else if (options && g_svc == S_READY) {
        // Options открывает меню из любого места (как и выбор вкладки меню верхней строки).
        g_focus = FOCUS_MENU; g_menuCursor = 0; g_redraw = 2;
    } else if (g_svc == S_FAILED || g_svc == S_STOPPED) {
        if (cross) { g_svc = S_CHECK; g_redraw = 2; }
    } else if (g_svc == S_READY) {
        int diskCount = (int)g_disks.disks.size();

        if (g_focus == FOCUS_MENU) {
            if (up || down) { g_menuCursor = up ? (g_menuCursor - 1 + MENU_ITEMS) % MENU_ITEMS : (g_menuCursor + 1) % MENU_ITEMS; g_redraw = 2; }
            else if (circle) { g_focus = FOCUS_TABS; g_redraw = 2; }
            else if (cross) {
                if (g_menuCursor == 0) {
                    service_refresh_disks(); g_nextDisks = now + 1500000ULL; g_disksOk = false;
                    g_focus = FOCUS_TABS;
                } else if (g_menuCursor == 1) {
                    i18n_set(i18n_current() == "ru" ? "en" : "ru");
                    // Меню остаётся открытым - можно сразу увидеть новую подпись и переключить обратно.
                } else if (g_menuCursor == 2) {
                    g_showDonate = true; g_focus = FOCUS_TABS;
                } else {
                    g_confirm = CONF_QUIT; g_focus = FOCUS_TABS;
                }
                g_redraw = 2;
            }
        } else if (g_focus == FOCUS_TABS) {
            int total = diskCount + 1; // +1 - вкладка меню
            if ((left || right) && total > 0) {
                g_tabCursor = left ? (g_tabCursor - 1 + total) % total : (g_tabCursor + 1) % total;
                g_redraw = 2;
            } else if (cross) {
                if (g_tabCursor < diskCount) {
                    g_selId = g_disks.disks[(size_t)g_tabCursor].id;
                    g_scanMsg.clear();
                } else {
                    g_focus = FOCUS_MENU; g_menuCursor = 0;
                }
                g_redraw = 2;
            } else if (down && !g_selId.empty()) {
                const DiskView* d = sel_disk();
                bool mineRunning = d && g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d->id;
                std::vector<ActKind> items = action_items(mineRunning);
                int idx = 0;
                for (size_t i = 0; i < items.size(); i++) if (items[i] == AK_STARTPAUSE) { idx = (int)i; break; }
                g_actionCursor = idx;
                g_focus = FOCUS_ACTIONS;
                g_redraw = 2;
            } else if (square) {
                service_refresh_disks(); g_nextDisks = now + 1500000ULL; g_disksOk = false; g_redraw = 2;
            }
        } else if (g_focus == FOCUS_ACTIONS) {
            const DiskView* d = sel_disk();
            bool mineRunning = d && g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d->id;
            std::vector<ActKind> items = action_items(mineRunning);
            int n = (int)items.size();
            // Circle здесь не используется (можно случайно задеть при игре с From/To) - назад
            // к вкладкам только по Up/Down, как и было задумано; Circle остаётся "назад" только
            // в меню и на окне с QR-кодом.
            if (up || down) { g_focus = FOCUS_TABS; g_redraw = 2; }
            else if ((left || right) && n > 0) {
                g_actionCursor = left ? (g_actionCursor - 1 + n) % n : (g_actionCursor + 1) % n;
                g_redraw = 2;
            } else if (cross && d && g_actionCursor >= 0 && g_actionCursor < n) {
                ActKind k = items[(size_t)g_actionCursor];
                if (k == AK_FROM) {
                    uint64_t v = g_rangeFromGb;
                    if (keyboard_edit_number(tr("kb_from", "From, GB"), v, 999999, v)) g_rangeFromGb = v;
                } else if (k == AK_TO) {
                    uint64_t v = g_rangeToGb;
                    if (keyboard_edit_number(tr("kb_to", "To, GB"), v, 999999, v)) g_rangeToGb = v;
                } else if (k == AK_WHOLE) {
                    g_rangeFromGb = 0; g_rangeToGb = 0;
                } else if (k == AK_STARTPAUSE) {
                    if (mineRunning) {
                        if (g_state.scan.state == "paused") service_scan_resume(); else service_scan_pause();
                        g_nextState = now;
                    } else if (!g_scanBusy && g_state.job == "idle") {
                        g_scanBusy = true; g_scanMsg.clear();
                        g_confirmFromGb = g_rangeFromGb; g_confirmToGb = g_rangeToGb;
                        service_scan_estimate(d->id, g_rangeFromGb, g_rangeToGb);
                        g_nextState = now;
                    }
                } else if (k == AK_STOP) {
                    g_confirm = CONF_SCAN_STOP;
                }
                g_redraw = 2;
            }
        }
    }

    static uint64_t lastTick = 0;
    if (now - lastTick >= 1000000) { lastTick = now; g_redraw = 2; }

    if (g_redraw > 0) {
        draw_all(now);
        g_scene->SubmitFlip(g_frameID);
        g_scene->FrameWait(g_frameID);
        g_scene->FrameBufferSwap();
        g_frameID++;
        g_redraw--;
        static bool hidden = false;
        if (!hidden) { hidden = true; int r = sceSystemServiceHideSplashScreen(); logf_("ui: HideSplashScreen = 0x%08X", (unsigned)r); }
    }
}
