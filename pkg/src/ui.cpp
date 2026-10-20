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
#include <string>
#include <vector>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

extern "C" uint64_t sceKernelGetProcessTime(void);
extern "C" int sceSystemServiceHideSplashScreen(void);

#ifndef APP_VERSION
#define APP_VERSION "0.0.0.0"
#endif

#define FRAME_WIDTH   1920
#define FRAME_HEIGHT  1080
#define FRAME_DEPTH      4

static const int FONT_PX   = 26;
static const int ROW_H     = 64;
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
static const int ICON_S = 26;   // сторона иконки в пикселях

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
        if (toks[i].isIcon) { draw_icon(toks[i].icon, x, baseline - 22, bg); x += ICON_S; }
        else { draw_text(toks[i].text, x, baseline, fg, bg); x += count_cp(toks[i].text) * g_cellW; }
    }
}

static int col_x(int col) { return MARGIN_X + col * g_cellW; }

static void put_left(int col, const std::string& s, int baseline, Color fg, Color bg)
{
    draw_text(s, col_x(col), baseline, fg, bg);
}

static void put_right(int col, int width, const std::string& s, int baseline, Color fg, Color bg)
{
    std::string t = fit(s, width);
    int n = count_cp(t);
    draw_text(t, col_x(col + width - n), baseline, fg, bg);
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

// ---------------------------------------------------------------- экраны: список дисков / SMART / сканирование

enum Screen { SCR_LIST, SCR_DISK, SCR_SCAN };
static Screen g_screen = SCR_LIST;
static std::string g_selId;       // id выбранного диска (не индекс - список может пересортироваться)

enum Confirm { CONF_NONE, CONF_QUIT, CONF_SCAN_START, CONF_SCAN_STOP };
static Confirm g_confirm = CONF_NONE;

static StateView   g_state;
static uint64_t    g_nextState = 0;
static bool        g_scanBusy = false;   // идёт замер скорости (между estimate и подтверждением)
static std::string g_scanMsg;            // временное сообщение экрана сканирования (замер, ошибка)
static EstView      g_confirmEst;        // результат замера, показанный в окне подтверждения старта

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

static void draw_panel(int w, int h)
{
    int x = (FRAME_WIDTH - w) / 2, y = (FRAME_HEIGHT - h) / 2;
    fill(x - 3, y - 3, w + 6, h + 6, C_EDGE);
    fill(x, y, w, h, C_PANEL);
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

// ---------------------------------------------------------------- экран: список дисков

static void draw_screen_list(int y0)
{
    if (!g_disksOk) { draw_text(tr("loading_disks", "Reading disks…"), MARGIN_X, y0 + 30, C_DIM, C_BG); return; }
    if (g_disks.disks.empty()) { draw_text(tr("no_disks", "No disks found"), MARGIN_X, y0 + 30, C_DIM, C_BG); return; }

    for (size_t i = 0; i < g_disks.disks.size(); i++) {
        const DiskView& d = g_disks.disks[i];
        int y = y0 + (int)i * ROW_H;
        if (y + ROW_H > 960) break;
        bool isSel = (d.id == g_selId);
        Color bg = (i & 1) ? C_ROW2 : C_PANEL;
        if (isSel) {
            fill(MARGIN_X - 10, y, FRAME_WIDTH - 2 * (MARGIN_X - 10), ROW_H - 6, C_EDGE);
            fill(MARGIN_X - 7, y + 3, FRAME_WIDTH - 2 * (MARGIN_X - 10) - 6, ROW_H - 12, bg);
        } else {
            fill(MARGIN_X - 10, y, FRAME_WIDTH - 2 * (MARGIN_X - 10), ROW_H - 6, bg);
        }
        int base = y + 38;
        draw_text(short_model(d), MARGIN_X, base, C_TEXT, bg);
        std::string sub = (d.label.empty() ? "" : d.label + " · ") + d.id + " · " + fmt_size(d.bytes);
        draw_text(sub, MARGIN_X + 520, base, C_DIM, bg);

        std::string vt; Color vc = C_DIM;
        if (g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d.id) {
            uint64_t tot = g_state.scan.to - g_state.scan.from;
            double pct = tot > 0 ? 100.0 * (double)g_state.scan.done / (double)tot : 0;
            char b[32]; snprintf(b, sizeof(b), "%s %.0f%%", g_state.scan.state == "paused" ? "||" : ">", pct);
            vt = b; vc = C_AMBER;
        } else if (!d.hasSmart || d.verdict < 0) vt = tr("vn", "No SMART");
        else if (d.verdict == 0) { vt = tr("v0", "Healthy"); vc = C_GREEN; }
        else if (d.verdict == 1) { vt = tr("v1", "Caution"); vc = C_AMBER; }
        else { vt = tr("v2", "Bad"); vc = C_RED; }
        if (d.hasSmart && d.temp > 0) { char t[16]; snprintf(t, sizeof(t), "  %d°C", d.temp); vt += t; }
        draw_text(vt, MARGIN_X + 1100, base, vc, bg);
    }
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

static void draw_screen_disk(const DiskView& d, int y0)
{
    std::string header = (!d.model.empty() ? d.model : !d.bridge.empty() ? d.bridge : d.id) + "   ·   " + fmt_size(d.bytes);
    draw_text(header, MARGIN_X, y0 + 34, C_TEXT, C_BG);

    if (!d.hasSmart) {
        draw_wrapped(tr("noSmartHint", "This disk does not provide SMART (common with USB enclosures). Surface scan is available."),
                     MARGIN_X, y0 + 90, 110, 2, C_DIM, C_BG);
        draw_text(tr("path", "Device") + ": " + d.path, MARGIN_X, y0 + 170, C_DIM, C_BG);
        return;
    }

    int y1 = y0 + 90;
    std::string vt; Color vc = C_DIM;
    if (d.verdict == 0) { vt = tr("v0", "Healthy"); vc = C_GREEN; }
    else if (d.verdict == 1) { vt = tr("v1", "Caution"); vc = C_AMBER; }
    else if (d.verdict == 2) { vt = tr("v2", "Bad"); vc = C_RED; }
    else vt = tr("vn", "No SMART");
    draw_text(vt, MARGIN_X, y1, vc, C_BG);
    if (d.temp > 0) { char t[32]; snprintf(t, sizeof(t), "%d °C", d.temp); draw_text(t, MARGIN_X + 280, y1, C_DIM, C_BG); }

    struct KV { std::string k, v; };
    std::vector<KV> left, right;
    left.push_back(KV{ tr("fw", "Firmware"), d.firmware.empty() ? "—" : d.firmware });
    left.push_back(KV{ tr("serial", "Serial number"), d.serial.empty() ? "—" : d.serial });
    left.push_back(KV{ tr("iface", "Interface"), d.proto + (d.bridge.empty() ? "" : (" · " + d.bridge)) });
    left.push_back(KV{ tr("rot", "Rotation rate"), d.rpm == 1 ? tr("ssd", "SSD") : d.rpm > 1 ? (fmt_num((uint64_t)d.rpm) + " " + tr("rpm", "rpm")) : "—" });
    right.push_back(KV{ tr("hr", "Total host reads"), d.hostRead >= 0 ? fmt_size((uint64_t)d.hostRead) : "—" });
    right.push_back(KV{ tr("hw", "Total host writes"), d.hostWrite >= 0 ? fmt_size((uint64_t)d.hostWrite) : "—" });
    right.push_back(KV{ tr("cycles", "Power cycles"), d.cycles >= 0 ? fmt_num((uint64_t)d.cycles) : "—" });
    right.push_back(KV{ tr("poh", "Power-on hours"), d.poh >= 0 ? fmt_num((uint64_t)d.poh) : "—" });

    int gy = y1 + 50;
    for (size_t i = 0; i < left.size(); i++) {
        draw_text(left[i].k + ":", MARGIN_X, gy + (int)i * 38, C_DIM, C_BG);
        draw_text(left[i].v, MARGIN_X + 340, gy + (int)i * 38, C_TEXT, C_BG);
    }
    for (size_t i = 0; i < right.size(); i++) {
        draw_text(right[i].k + ":", MARGIN_X + 900, gy + (int)i * 38, C_DIM, C_BG);
        draw_text(right[i].v, MARGIN_X + 1260, gy + (int)i * 38, C_TEXT, C_BG);
    }

    int y2 = gy + 4 * 38 + 24;
    if (d.reasons.empty() && d.verdict == 0) {
        draw_text(tr("ok", "No critical indicators found."), MARGIN_X, y2, C_GREEN, C_BG);
        y2 += 36;
    } else {
        size_t shown = 0;
        for (size_t i = 0; i < d.reasons.size() && shown < 3; i++, shown++) {
            Color rc = d.reasons[i].sev == 2 ? C_RED : C_AMBER;
            draw_text(fit(reason_text(d, d.reasons[i]), 150), MARGIN_X, y2, rc, C_BG);
            y2 += 32;
        }
    }

    y2 += 16;
    draw_text(tr("attrs", "SMART attributes"), MARGIN_X, y2, C_TEXT, C_BG);
    y2 += 34;
    put_left(0, tr("id", "ID"), y2, C_DIM, C_BG);
    put_left(6, tr("name", "Name"), y2, C_DIM, C_BG);
    put_right(60, 7, tr("cur", "Current"), y2, C_DIM, C_BG);
    put_right(68, 7, tr("worst", "Worst"), y2, C_DIM, C_BG);
    put_right(76, 7, tr("thr", "Thresh"), y2, C_DIM, C_BG);
    put_right(96, 14, tr("raw", "Raw"), y2, C_DIM, C_BG);
    y2 += 30;
    for (size_t i = 0; i < d.attrs.size(); i++) {
        if (y2 > 1000) break;
        const SmartAttrView& a = d.attrs[i];
        Color rc = a.sev == 2 ? C_RED : a.sev == 1 ? C_AMBER : C_TEXT;
        char idhex[8]; snprintf(idhex, sizeof(idhex), "%02X", (unsigned)a.id);
        put_left(0, idhex, y2, rc, C_BG);
        put_left(6, fit(a.name, 52), y2, C_TEXT, C_BG);
        put_right(60, 7, fmt_num((uint64_t)a.cur), y2, C_TEXT, C_BG);
        put_right(68, 7, fmt_num((uint64_t)a.worst), y2, C_TEXT, C_BG);
        put_right(76, 7, fmt_num((uint64_t)a.thresh), y2, C_TEXT, C_BG);
        put_right(96, 14, a.raw, y2, C_DIMMER, C_BG);
        y2 += 28;
    }
}

// ---------------------------------------------------------------- экран: проверка поверхности

static void draw_screen_scan(const DiskView& d, int y0)
{
    bool mine = g_state.scan.present && g_state.scan.disk == d.id;
    const ScanView& sc = g_state.scan;
    const std::string& job = g_state.job;

    std::string header = tr("scanTitle", "Surface scan") + "   ·   " + short_model(d) + " (" + d.id + ")";
    draw_text(header, MARGIN_X, y0 + 30, C_TEXT, C_BG);

    std::string msg;
    if (g_scanBusy) msg = tr("bench", "Measuring read speed…");
    else if (mine && job == "scan") msg = sc.state == "paused" ? tr("paused", "Paused") : sc.state == "stopping" ? tr("stopping", "Stopping…") : tr("running", "Scanning the surface…");
    else if (mine && !sc.msg.empty() && sc.state != "running") msg = tr(("m_" + sc.msg).c_str(), sc.msg.c_str());
    else if (!g_scanMsg.empty()) msg = g_scanMsg;
    else msg = tr("idle", "Ready. The scan only reads data and never writes.");
    draw_text(msg, MARGIN_X, y0 + 76, C_DIM, C_BG);

    double pct = 0;
    if (mine) { uint64_t tot = sc.to - sc.from; pct = tot > 0 ? 100.0 * (double)sc.done / (double)tot : 0; }
    int barY = y0 + 106, barW = FRAME_WIDTH - 2 * MARGIN_X, barH = 14;
    fill(MARGIN_X, barY, barW, barH, Color{ 30, 33, 40 });
    fill(MARGIN_X, barY, (int)((double)barW * pct / 100.0), barH, Color{ 91, 157, 255 });

    const int cols = 25, rows = 20, gap = 3, mapW = 1260, cellH = 26;
    int cellW = (mapW - (cols - 1) * gap) / cols;
    int mapY = barY + 36;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            int v = (mine && idx < (int)sc.map.size()) ? (sc.map[(size_t)idx] - '0') : 0;
            fill(MARGIN_X + c * (cellW + gap), mapY + r * (cellH + gap), cellW, cellH, hist_color(v));
        }
    }
    int mapH = rows * (cellH + gap) - gap;

    int legX = MARGIN_X + mapW + 30, legW = FRAME_WIDTH - MARGIN_X - legX;
    int legRowH = mapH / 7;
    for (int i = 1; i <= 7; i++) {
        int ly = mapY + (i - 1) * legRowH;
        fill(legX, ly, legW, legRowH - 6, C_PANEL);
        fill(legX + 10, ly + (legRowH - 6) / 2 - 10, 20, 20, hist_color(i));
        char key[4]; snprintf(key, sizeof(key), "h%d", i);
        draw_text(tr(key, ""), legX + 44, ly + (legRowH - 6) / 2 + 8, C_TEXT, C_PANEL);
        std::string cs = fmt_num(mine ? sc.hist[i] : 0);
        draw_text(cs, legX + legW - 24 - (int)cs.size() * g_cellW, ly + (legRowH - 6) / 2 + 8, C_DIM, C_PANEL);
    }

    int statY = mapY + mapH + 30;
    struct St { std::string k, v; };
    std::vector<St> stats;
    { char b[32]; snprintf(b, sizeof(b), "%.1f %%", pct); stats.push_back(St{ tr("prog", "Progress"), b }); }
    stats.push_back(St{ tr("speed", "Speed"), (mine && sc.speed > 0) ? (fmt_num((uint64_t)(sc.speed / 1e6)) + " MB/s") : "—" });
    stats.push_back(St{ tr("elapsed", "Elapsed"), mine ? fmt_time(sc.elapsed) : "—" });
    stats.push_back(St{ tr("left", "Remaining"), (mine && job == "scan" && sc.eta >= 0) ? fmt_time((double)sc.eta) : "—" });
    stats.push_back(St{ tr("pos", "Position"), mine ? fmt_size(sc.from + sc.done) : "—" });
    stats.push_back(St{ tr("bad", "Bad sectors"), mine ? (fmt_num(sc.bad) + (sc.approx ? " *" : "")) : "—" });
    int sw = (FRAME_WIDTH - 2 * MARGIN_X) / 3;
    for (size_t i = 0; i < stats.size(); i++) {
        int cx = MARGIN_X + (int)(i % 3) * sw, cy = statY + (int)(i / 3) * 70;
        fill(cx, cy, sw - 10, 60, C_PANEL);
        draw_text(stats[i].k, cx + 14, cy + 24, C_DIM, C_PANEL);
        draw_text(stats[i].v, cx + 14, cy + 50, C_TEXT, C_PANEL);
    }
}

static void draw_all(uint64_t now)
{
    fill(0, 0, FRAME_WIDTH, FRAME_HEIGHT, C_BG);

    if (g_bannerOk) draw_image(g_banner, BANNER_LEFT, BANNER_TOP, g_bannerW, BANNER_H, C_BG);
    else draw_text("PS4 HDD TOOLS", MARGIN_X, BANNER_TOP + 44, C_TEXT, C_BG);

    // Состояние службы (справа сверху).
    std::string st; Color sc = C_DIM;
    switch (g_svc) {
    case S_CHECK:   st = tr("svc_check", "Looking for the service…"); break;
    case S_WAITING: st = tr("svc_waiting", "Waiting for the service to start…"); break;
    case S_READY:   st = tr("svc_ready", "Service is running"); sc = C_GREEN; break;
    case S_FAILED:  st = tr("svc_failed", "Could not start the service"); sc = C_RED; break;
    case S_STOPPED: st = tr("svc_stopped", "Service is shut down"); sc = C_AMBER; break;
    }
    draw_text(st, FRAME_WIDTH - MARGIN_X - count_cp(st) * g_cellW, BANNER_TOP + 30, sc, C_BG);
    if (g_svc == S_READY) {
        std::string v = "v" + g_svcVersion;
        draw_text(v, FRAME_WIDTH - MARGIN_X - (int)v.size() * g_cellW, BANNER_TOP + 60, C_DIMMER, C_BG);
    }

    int y0 = BANNER_TOP + BANNER_H + 30;
    std::string hint;

    if (g_svc == S_FAILED) {
        draw_text(g_svcErr, MARGIN_X, y0 + 30, C_RED, C_BG);
        draw_text(tr("svc_hint_fail", "Make sure GoldHEN is running (BinLoader, port 9090). Cross - try again"), MARGIN_X, y0 + 80, C_DIM, C_BG);
    } else if (g_svc == S_STOPPED) {
        draw_text(tr("svc_hint_stopped", "Cross - start the service again"), MARGIN_X, y0 + 30, C_DIM, C_BG);
    } else if (g_svc == S_READY) {
        const DiskView* d = sel_disk();
        if (g_screen == SCR_DISK && d) {
            draw_screen_disk(*d, y0);
            hint = "{circle} " + tr("hint_back", "back") + "     {cross} " + tr("hint_scan", "surface scan");
        } else if (g_screen == SCR_SCAN && d) {
            draw_screen_scan(*d, y0);
            bool mineRunning = g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d->id;
            if (mineRunning) {
                std::string pr = (g_state.scan.state == "paused") ? tr("hint_resume", "resume") : tr("hint_pause", "pause");
                hint = "{cross} " + pr + "     {triangle} " + tr("hint_stop", "stop") + "     {circle} " + tr("hint_back", "back");
            } else {
                hint = "{cross} " + tr("hint_start", "start scan") + "     {circle} " + tr("hint_back", "back");
            }
        } else {
            g_screen = SCR_LIST;
            draw_screen_list(y0);
            if (!g_disks.disks.empty())
                hint = "{updown} " + tr("hint_select", "select disk") + "     {cross} " + tr("hint_open", "open disk") + "     {square} " + tr("hint_refresh", "refresh disks") + "     {options} " + tr("hint_quit", "shut down the service");
            else
                hint = "{square} " + tr("hint_refresh", "refresh disks") + "     {options} " + tr("hint_quit", "shut down the service");
        }
    }

    for (size_t i = 0; i < g_diagLines.size(); i++) draw_text(g_diagLines[i].substr(0, 130), MARGIN_X, 900 + (int)i * 30, C_DIMMER, C_BG);

    if (!hint.empty() && g_confirm == CONF_NONE) put_center_rich(FRAME_WIDTH / 2, hint, 1010, C_DIM, C_BG);
    std::string credit = "PS4 HDD Tools v" + version_str() + " Created by SergioPoverony and Mr.Claude";
    draw_text(credit, (FRAME_WIDTH - count_cp(credit) * g_cellW) / 2, 1050, C_DIMMER, C_BG);

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

static int g_pad = -1;

static bool pad_init()
{
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams param;
    param.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&param);
    int userID = 0;
    sceUserServiceGetInitialUser(&userID);
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
    bool triangle = (pressed & ORBIS_PAD_BUTTON_TRIANGLE) != 0;
    bool square   = (pressed & ORBIS_PAD_BUTTON_SQUARE) != 0;
    bool options  = (pressed & ORBIS_PAD_BUTTON_OPTIONS) != 0;
    bool up       = (pressed & ORBIS_PAD_BUTTON_UP) != 0;
    bool down     = (pressed & ORBIS_PAD_BUTTON_DOWN) != 0;

    if (g_confirm != CONF_NONE) {
        if (cross) {
            if (g_confirm == CONF_QUIT) {
                service_quit();
                g_svc = S_STOPPED; g_disksOk = false; g_screen = SCR_LIST;
            } else if (g_confirm == CONF_SCAN_START) {
                const DiskView* d = sel_disk();
                if (d) { service_scan_start(d->id, 0, 0); g_nextState = now; }
            } else if (g_confirm == CONF_SCAN_STOP) {
                service_scan_stop(); g_nextState = now;
            }
            g_confirm = CONF_NONE; g_redraw = 2;
        } else if (circle) { g_confirm = CONF_NONE; g_redraw = 2; }
    } else if (options && g_svc == S_READY) {
        g_confirm = CONF_QUIT; g_redraw = 2;
    } else if (g_svc == S_FAILED || g_svc == S_STOPPED) {
        if (cross) { g_svc = S_CHECK; g_redraw = 2; }
    } else if (g_svc == S_READY) {
        if (g_screen == SCR_LIST) {
            if ((up || down) && !g_disks.disks.empty()) {
                int i = sel_idx(); int n = (int)g_disks.disks.size();
                if (i < 0) i = 0; else i = up ? (i - 1 + n) % n : (i + 1) % n;
                g_selId = g_disks.disks[(size_t)i].id; g_redraw = 2;
            } else if (cross) {
                if (sel_disk()) { g_screen = SCR_DISK; g_redraw = 2; }
            } else if (square) {
                service_refresh_disks(); g_nextDisks = now + 1500000ULL; g_disksOk = false; g_redraw = 2;
            }
        } else if (g_screen == SCR_DISK) {
            if (circle) { g_screen = SCR_LIST; g_redraw = 2; }
            else if (cross) { g_screen = SCR_SCAN; g_scanMsg.clear(); g_redraw = 2; }
        } else if (g_screen == SCR_SCAN) {
            const DiskView* d = sel_disk();
            bool mineRunning = d && g_state.scan.present && g_state.job == "scan" && g_state.scan.disk == d->id;
            if (circle) { g_screen = SCR_DISK; g_redraw = 2; }
            else if (triangle && mineRunning) { g_confirm = CONF_SCAN_STOP; g_redraw = 2; }
            else if (cross && d) {
                if (mineRunning) {
                    if (g_state.scan.state == "paused") service_scan_resume(); else service_scan_pause();
                    g_nextState = now;
                } else if (!g_scanBusy && g_state.job == "idle") {
                    g_scanBusy = true; g_scanMsg.clear();
                    service_scan_estimate(d->id, 0, 0);
                    g_nextState = now;
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
