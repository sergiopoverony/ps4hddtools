// Проверка загрузки переводов приложением PKG на ПК (его настоящими i18n.cpp и json.cpp).
// Из корня проекта:
//   g++ -std=c++17 -Wall -Wextra -Ipkg/src -o build/test_pkg_i18n tests/test_pkg_i18n.cpp pkg/src/i18n.cpp pkg/src/json.cpp
//   mkdir -p build/i18n_test && cp lang/lang_*.json build/i18n_test/
//   python3 tools/langs.py lang build/i18n_test/languages.json
//   build/test_pkg_i18n build/i18n_test
// (в PKG те же файлы кладёт pkg/Makefile в assets/misc/)
#include "i18n.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

static int g_sysLang = 1;
extern "C" int sceSystemServiceParamGetInt(int, int* v) { *v = g_sysLang; return 0; }
void logf_(const char*, ...) {}   // журнал в тесте не нужен

static int fails = 0;
#define CHECK(name, cond) do { bool ok_ = (cond); printf("%s %s\n", ok_ ? "PASS" : "FAIL", name); if (!ok_) fails++; } while (0)

int main(int argc, char** argv)
{
    std::string dir = argc > 1 ? argv[1] : "build/i18n_test";

    g_sysLang = 8;   // русский
    i18n_init(dir.c_str());
    CHECK("русская консоль -> ru", i18n_current() == "ru");
    CHECK("ru: menu_refresh", tr("menu_refresh", "?") == "Обновить диски");
    CHECK("ru: _name", tr("_name", "?") == "Русский");

    g_sysLang = 4;   // немецкий
    i18n_init(dir.c_str());
    CHECK("немецкая консоль -> de", i18n_current() == "de");
    CHECK("de: _name", tr("_name", "?") == "Deutsch");

    g_sysLang = 17;  // португальский бразильский: файла pt-br нет, запасной pt
    i18n_init(dir.c_str());
    CHECK("pt-br -> запасной pt", i18n_current() == "pt");

    g_sysLang = 0;   // японский: файла нет -> английский
    i18n_init(dir.c_str());
    CHECK("японская консоль -> en", i18n_current() == "en");
    CHECK("en: menu_refresh", tr("menu_refresh", "?") == "Refresh disks");

    g_sysLang = 27;  // тайский: файла нет -> английский
    i18n_init(dir.c_str());
    CHECK("тайская консоль -> en", i18n_current() == "en");

    // список языков и переключение по кругу
    const std::vector<LangInfo>& langs = i18n_languages();
    int n = (int)langs.size();
    CHECK("в списке 20 языков", n == 20);
    CHECK("первые два en, ru", n >= 2 && langs[0].code == "en" && langs[1].code == "ru");
    i18n_set("en");
    for (int i = 0; i < n; i++) i18n_next(+1);
    CHECK("n раз вперёд - снова en", i18n_current() == "en");
    i18n_next(-1);
    CHECK("назад с en -> последний язык списка", i18n_current() == langs[n - 1].code);
    i18n_set("ru");
    i18n_next(-1);
    CHECK("назад с ru -> en", i18n_current() == "en");

    // каждый язык грузится и отдаёт строки (в том числе общие с английским запасным)
    bool all = true;
    for (int i = 0; i < n; i++) {
        i18n_set(langs[i].code.c_str());
        if (i18n_current() != langs[i].code) { all = false; printf("  не загрузился %s\n", langs[i].code.c_str()); continue; }
        if (tr("_name", "") != langs[i].name) { all = false; printf("  _name %s\n", langs[i].code.c_str()); }
        if (tr("menu_lang", "").empty() || tr("cTitle", "").find("{d}") == std::string::npos) { all = false; printf("  пустые строки %s\n", langs[i].code.c_str()); }
    }
    CHECK("все языки загружаются и дают строки", all);

    // несуществующий язык: остаётся прежний
    i18n_set("de");
    i18n_set("xx");
    CHECK("неизвестный код не меняет язык", i18n_current() == "de" && tr("_name", "?") == "Deutsch");

    // папки с файлами нет: два запасных языка, текст по умолчанию из кода
    i18n_init("/nonexistent");
    CHECK("нет папки: список из двух запасных языков", i18n_languages().size() == 2);
    CHECK("нет папки: en и текст по умолчанию", i18n_current() == "en" && tr("menu_refresh", "dflt") == "dflt");

    printf("\nИТОГО: провалено %d\n", fails);
    return fails ? 1 : 0;
}
