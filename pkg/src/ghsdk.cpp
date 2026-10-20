#include "ghsdk.h"

// Как в GoldHEN SDK (orbis_syscall(500, cmd, data)): косвенный вызов syscall(0, 500, cmd, data).
// Соглашение FreeBSD: rax = 0 (косвенный), rdi = номер, rsi = cmd, rdx = data; флаг переноса = ошибка.
static long gh_cmd(long cmd, void* data, bool* ok)
{
    long ret;
    unsigned char carry;
    __asm__ volatile(
        "syscall\n\t"
        "setc %1"
        : "=a"(ret), "=q"(carry)
        : "a"(0L), "D"(500L), "S"(cmd), "d"(data)
        : "rcx", "r11", "memory", "cc");
    if (ok) *ok = (carry == 0);
    return ret;
}

long gh_version(bool* ok) { return gh_cmd(0, nullptr, ok); }
long gh_jailbreak(jailbreak_backup* jb, bool* ok) { return gh_cmd(2, jb, ok); }
long gh_unjailbreak(jailbreak_backup* jb, bool* ok) { return gh_cmd(3, jb, ok); }
