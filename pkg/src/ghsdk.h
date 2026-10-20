#pragma once
#include <stdint.h>

// Системный вызов GoldHEN SDK (номер 500, см. GoldHEN Plugin SDK, лицензия MIT).
// Команды: 0 версия, 2 jailbreak, 3 unjailbreak. Используем только их.
struct jailbreak_backup {
    uint32_t cr_uid;
    uint32_t cr_ruid;
    uint32_t cr_rgid;
    uint32_t cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void* cr_prison;
    void* fd_cdir;
    void* fd_jdir;
    void* fd_rdir;
};

// Возвращают результат вызова; ok = false, если ядро ответило ошибкой (например, такого вызова нет).
long gh_version(bool* ok);
long gh_jailbreak(jailbreak_backup* jb, bool* ok);
long gh_unjailbreak(jailbreak_backup* jb, bool* ok);
