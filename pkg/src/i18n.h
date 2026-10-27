#pragma once
#include <string>
#include <vector>

// Тексты интерфейса. Язык берётся из системных настроек консоли; если для него нет файла, английский.
// Файлы lang_<код>.json общие с веб-страницей службы (папка lang/ в корне проекта), сборка кладёт их
// в PKG вместе с languages.json (список языков). Если ключа нет, берётся английский текст из lang_en.json.

void i18n_init(const char* assetDir = "/app0/assets/misc");
std::string tr(const char* key, const char* def);

// Переключение языка из меню приложения (помимо автоопределения по консоли при старте).
// code - код из списка i18n_languages(). Если встроенного файла для этого кода нет, язык не меняется.
void i18n_set(const char* code);
void i18n_next(int dir);          // следующий (+1) или предыдущий (-1) язык по кругу
std::string i18n_current();       // код текущего языка

struct LangInfo { std::string code; std::string name; };   // name - родное название ("Русский")
const std::vector<LangInfo>& i18n_languages();
