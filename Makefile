# Сборка: export PS4_PAYLOAD_SDK=/путь/к/sdk; make
# Отправка: make send PS4_HOST=192.168.0.24
ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else ifneq ($(wildcard /opt/ps4-payload-sdk/toolchain/orbis.mk),)
    PS4_PAYLOAD_SDK := /opt/ps4-payload-sdk
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

# Экспортируем переменную окружения PS4_PAYLOAD_SDK для дочерних процессов:
# скрипт orbis-ld из ps4-payload-sdk сам не вычисляет свой путь (баг/опечатка
# в самом SDK - там написано ${PS4_PAYLOAD_SDK}, хотя локально переменная
# называется иначе), и если PS4_PAYLOAD_SDK не задана в окружении, линковщик
# получает путь к линкер-скрипту без префикса ("/ldscripts/elf_x86_64.x"
# вместо "/opt/ps4-payload-sdk/ldscripts/elf_x86_64.x") и падает с ошибкой
# "cannot find linker script". export здесь гарантирует, что переменная
# дойдёт до orbis-ld, даже если пользователь не экспортировал её сам.
export PS4_PAYLOAD_SDK

PS4_HOST ?= 192.168.0.24
PS4_PORT ?= 9090
TARGET   := build/ps4hddtools.elf
SOURCES  := $(wildcard src/*.cpp)
HEADERS  := $(wildcard src/*.h)
ASSETS   := build/assets_gen.h
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -lSceSystemService

all: $(TARGET)

$(ASSETS): web/index.html assets/title.png assets/icon.png assets/qr_sbp.png assets/qr_paypal.png src/version.h tools/embed.py $(wildcard lang/lang_*.json)
	mkdir -p build
	python3 tools/embed.py $(ASSETS)

$(TARGET): $(SOURCES) $(HEADERS) $(ASSETS)
	$(CXX) $(CXXFLAGS) -Isrc -Ibuild -o $@ $(SOURCES)

send: $(TARGET)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(TARGET)

clean:
	rm -rf build

.PHONY: all clean send
