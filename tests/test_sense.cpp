// Сборка: g++ -std=c++17 -DHOST_TEST -Isrc tests/test_sense.cpp src/smart.cpp src/log.cpp src/platform.cpp -pthread -o build/test_sense
#include "smart.h"
#include <cstdio>
#include <cstring>
static int fails = 0;
static void check(const char* n, bool ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", n); if (!ok) ++fails; }
static void mk(uint8_t* s, uint8_t mid, uint8_t high) {  // sense 0x72 с дескриптором ATA Status Return
    memset(s, 0, 32);
    s[0] = 0x72; s[1] = 0x01; s[3] = 0x1D; s[7] = 14;
    s[8] = 0x09; s[9] = 0x0C; s[17] = mid; s[19] = high; s[21] = 0x50;
}
int main() {
    uint8_t s[32];
    mk(s, 0x4F, 0xC2); check("исправен (4F/C2)", smart_status_from_sense(s, 32) == 0);
    mk(s, 0xF4, 0x2C); check("скоро откажет (F4/2C)", smart_status_from_sense(s, 32) == 1);
    mk(s, 0x00, 0x00); check("иное значение -> неизвестно", smart_status_from_sense(s, 32) == -1);
    mk(s, 0x4F, 0xC2); check("буфер 18 байт слишком мал (прежняя ошибка)", smart_status_from_sense(s, 18) == -1);
    mk(s, 0x4F, 0xC2); s[0] = 0x70; check("фиксированный формат не разбирается", smart_status_from_sense(s, 32) == -1);
    mk(s, 0x4F, 0xC2); s[8] = 0x00; check("нет дескриптора 0x09", smart_status_from_sense(s, 32) == -1);
    check("nullptr", smart_status_from_sense(nullptr, 32) == -1);
    printf("ИТОГО: провалено %d\n", fails);
    return fails ? 1 : 0;
}
