#pragma once
#include <string>

// Тексты интерфейса. Язык берётся из системных настроек консоли: русский или английский.
// Встроенные файлы assets/misc/lang_<код>.json лежат в PKG. Если ключа нет, используется текст по умолчанию (английский).

void i18n_init(const char* assetDir = "/app0/assets/misc");
std::string tr(const char* key, const char* def);

// Переключение языка из меню приложения (помимо автоопределения по консоли при старте).
// code - "ru" или "en". Если встроенного файла для этого кода нет, язык не меняется.
void i18n_set(const char* code);
std::string i18n_current();
