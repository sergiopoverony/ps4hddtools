#include "i18n.h"
#include "json.h"
#include "log.h"

#include <map>
#include <vector>
#include <stdio.h>

// Язык системы PS4: sceSystemServiceParamGetInt(SCE_SYSTEM_SERVICE_PARAM_ID_LANG, &id).
extern "C" int sceSystemServiceParamGetInt(int paramId, int* value);
static const int PARAM_ID_LANG = 1;

static std::string g_want = "en";     // код текущего языка ("ru", "de", ...)
static std::string g_dir = "/app0/assets/misc";
static std::map<std::string, std::string> g_strings;
static std::vector<LangInfo> g_langs;   // языки из languages.json, который кладёт в PKG сборка

static void merge_json(const JsonValue& j)
{
    for (size_t i = 0; i < j.obj.size(); i++)
        if (j.obj[i].second.type == JsonValue::STR) g_strings[j.obj[i].first] = j.obj[i].second.str;
}

static bool read_json_file(const std::string& path, JsonValue& j)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string body;
    char buf[2048];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    fclose(f);
    return json_parse(body, j);
}

static bool load_bundled(const std::string& path)
{
    JsonValue j;
    if (!read_json_file(path, j) || j.type != JsonValue::OBJ) return false;
    merge_json(j);
    return true;
}

// Загружает язык: сначала английский (запасной вариант для ключей, которых нет в переводе),
// поверх него выбранный. Файлы lang_<код>.json общие с веб-страницей (папка lang/ в корне проекта).
static bool load_language(const std::string& code)
{
    std::map<std::string, std::string> saved = g_strings;
    g_strings.clear();
    load_bundled(g_dir + "/lang_en.json");
    if (code != "en" && !load_bundled(g_dir + "/lang_" + code + ".json")) {
        g_strings = saved;
        return false;
    }
    return true;
}

static void load_index()
{
    g_langs.clear();
    JsonValue j;
    if (read_json_file(g_dir + "/languages.json", j) && j.type == JsonValue::ARR) {
        for (size_t i = 0; i < j.arr.size(); i++) {
            LangInfo li;
            li.code = j.arr[i]["code"].str;
            li.name = j.arr[i]["name"].str;
            if (!li.code.empty()) g_langs.push_back(li);
        }
    }
    if (g_langs.empty()) {   // файла нет (старая сборка) - хотя бы два основных языка
        LangInfo en; en.code = "en"; en.name = "English"; g_langs.push_back(en);
        LangInfo ru; ru.code = "ru"; ru.name = "Русский"; g_langs.push_back(ru);
    }
}

// ID языка PS4 (SCE_SYSTEM_PARAM_LANG) -> коды файлов lang_<код>.json по порядку предпочтения.
// Сначала точный вариант (pt-br), затем общий (pt). Положите файл assets/misc/lang_<код>.json
// и язык подхватится сам, менять код не нужно.
static std::vector<std::string> lang_candidates(int id)
{
    std::vector<std::string> c;
    switch (id) {
    case 0:  c.push_back("ja"); break;
    case 1:  c.push_back("en"); break;
    case 2:  c.push_back("fr"); break;
    case 3:  c.push_back("es"); break;
    case 4:  c.push_back("de"); break;
    case 5:  c.push_back("it"); break;
    case 6:  c.push_back("nl"); break;
    case 7:  c.push_back("pt"); break;
    case 8:  c.push_back("ru"); break;
    case 9:  c.push_back("ko"); break;
    case 10: c.push_back("zh-hant"); c.push_back("zh"); break;
    case 11: c.push_back("zh-hans"); c.push_back("zh"); break;
    case 12: c.push_back("fi"); break;
    case 13: c.push_back("sv"); break;
    case 14: c.push_back("da"); break;
    case 15: c.push_back("no"); break;
    case 16: c.push_back("pl"); break;
    case 17: c.push_back("pt-br"); c.push_back("pt"); break;
    case 18: c.push_back("en-gb"); c.push_back("en"); break;
    case 19: c.push_back("tr"); break;
    case 20: c.push_back("es-la"); c.push_back("es"); break;
    case 21: c.push_back("ar"); break;
    case 22: c.push_back("fr-ca"); c.push_back("fr"); break;
    case 23: c.push_back("cs"); break;
    case 24: c.push_back("hu"); break;
    case 25: c.push_back("el"); break;
    case 26: c.push_back("ro"); break;
    case 27: c.push_back("th"); break;
    case 28: c.push_back("vi"); break;
    case 29: c.push_back("id"); break;
    default: c.push_back("en"); break;
    }
    return c;
}

void i18n_init(const char* assetDir)
{
    g_dir = assetDir;
    load_index();
    int id = -1;
    int r = sceSystemServiceParamGetInt(PARAM_ID_LANG, &id);
    if (r != 0) id = -1;
    std::vector<std::string> cands = lang_candidates(id);
    logf_("i18n: system language id %d (rc 0x%08X), candidates: %s%s", id, (unsigned)r,
          cands[0].c_str(), cands.size() > 1 ? (" , " + cands[1]).c_str() : "");

    // Берём первый вариант, для которого есть встроенный файл. Если файла нет ни для одного,
    // остаётся английский.
    g_want = "en";
    bool ok = false;
    for (size_t i = 0; i < cands.size() && !ok; i++) {
        if (load_language(cands[i])) { g_want = cands[i]; ok = true; }
    }
    if (!ok) load_language("en");
    logf_("i18n: using '%s'%s, %d languages available (%d strings)", g_want.c_str(),
          ok ? "" : " (no file for the system language, English defaults)", (int)g_langs.size(), (int)g_strings.size());
}

std::string tr(const char* key, const char* def)
{
    std::map<std::string, std::string>::const_iterator it = g_strings.find(key);
    if (it != g_strings.end() && !it->second.empty()) return it->second;
    return def;
}

void i18n_set(const char* code)
{
    if (load_language(code)) {
        g_want = code;
        logf_("i18n: switched to '%s' from menu", code);
    } else {
        logf_("i18n: no bundled file for '%s', keeping '%s'", code, g_want.c_str());   // язык не меняем
    }
}

void i18n_next(int dir)
{
    if (g_langs.empty()) return;
    int n = (int)g_langs.size(), cur = 0;
    for (int i = 0; i < n; i++) if (g_langs[i].code == g_want) { cur = i; break; }
    i18n_set(g_langs[((cur + dir) % n + n) % n].code.c_str());
}

const std::vector<LangInfo>& i18n_languages() { return g_langs; }

std::string i18n_current() { return g_want; }
