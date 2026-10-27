# Сборка PKG приложения PS4 HDD Tools (оболочка для службы ps4hddtools).
# Нужен OpenOrbis PS4 Toolchain и переменная OO_PS4_TOOLCHAIN. Сборка: make (в этой папке).
# Перед сборкой соберите службу в корне проекта (make), чтобы был ../release/ps4hddtools.elf.
#
# Частая ошибка - после правки версии/кода службы собрать только тут (make) и получить PKG
# со старым (не пересобранным) payload внутри, хотя сам интерфейс приложения уже новый.
# Чтобы собрать и службу, и приложение одной командой и гарантированно свежими:
#   make full
# (нужен и PS4_PAYLOAD_SDK для службы в корне проекта, и OO_PS4_TOOLCHAIN для этой сборки -
# оба должны быть экспортированы заранее, как обычно).

# PkgTool.Core работает на .NET: без этих настроек он падает, если в системе нет библиотеки ICU,
# или ругается на версию .NET.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1
export DOTNET_ROLL_FORWARD := LatestMajor

TITLE       := PS4 HDD Tools
TITLE_ID    := BREW00089
CONTENT_ID  := IV0000-BREW00089_00-PS4HDDTOOLS00000
LIBS        := -lc -lkernel -lc++ -lSceNet -lSceVideoOut -lSceSysmodule -lSceFreeType -lScePad -lSceUserService -lSceSystemService -lSceImeDialog -lSceCommonDialog
EXTRAFLAGS  := -O2

# Версия приложения = версия службы (из ../src/version.h): приложение сравнивает их при запуске.
APPVER      := $(shell sed -n 's/.*APP_VERSION "\(.*\)".*/\1/p' ../src/version.h)

# Переводы общие с веб-страницей службы: файлы лежат в ../lang/lang_<код>.json. Сборка копирует
# их в assets/misc/ и добавляет languages.json (список языков, делает tools/langs.py). Новый язык
# = новый файл ../lang/lang_<код>.json, этот Makefile менять не нужно.
LANGSRC     := $(wildcard ../lang/lang_*.json)
LANGDST     := $(patsubst ../lang/%,assets/misc/%,$(LANGSRC)) assets/misc/languages.json

ASSETS      := assets/fonts/DejaVuSansMono.ttf assets/fonts/LICENSE-DejaVu.txt assets/images/title.png \
               assets/images/qr_sbp.png assets/images/qr_paypal.png \
               $(LANGDST) assets/misc/ps4hddtools.elf

# Системные модули для приложения (без libc.prx приложение сразу вылетает). Берутся из набора OpenOrbis.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build/obj

CFILES      := $(wildcard $(PROJDIR)/*.c)
CPPFILES    := $(wildcard $(PROJDIR)/*.cpp)
OBJS        := $(patsubst $(PROJDIR)/%.c, $(INTDIR)/%.o, $(CFILES)) $(patsubst $(PROJDIR)/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

CFLAGS      := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -DAPP_VERSION=\"$(APPVER)\" -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
CXXFLAGS    := $(CFLAGS) -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR) sce_sys/about sce_module assets/misc)

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
		CC      := /usr/local/opt/llvm/bin/clang
		CCX     := /usr/local/opt/llvm/bin/clang++
		LD      := /usr/local/opt/llvm/bin/ld.lld
		CDIR    := macos
endif
ifneq (,$(findstring MSYS_NT,$(UNAME_S)))
		CC      := clang
		CCX     := clang++
		LD      := ld.lld
		CDIR    := windows
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

# Системные модули: ищем файл с нужным именем в наборе OpenOrbis (обычно он лежит в samples/*/sce_module).
sce_module/%.prx:
	@src="$$(find '$(TOOLCHAIN)' -name '$*.prx' 2>/dev/null | head -n 1)"; \
	if [ -z "$$src" ]; then echo "Не найден $*.prx в $(TOOLCHAIN). Скопируйте его вручную в pkg/sce_module/"; exit 1; fi; \
	echo "cp $$src $@"; cp "$$src" $@

assets/misc/lang_%.json: ../lang/lang_%.json
	cp $< $@

assets/misc/languages.json: $(LANGSRC) ../tools/langs.py
	python3 ../tools/langs.py ../lang $@

# Служба берётся из корня проекта.
assets/misc/ps4hddtools.elf: ../release/ps4hddtools.elf
	cp $< $@

# Иконка и фон запуска берутся из ../sce_sys (имя Icon0.png там с заглавной буквы, PS4 ждёт icon0.png).
sce_sys/icon0.png: ../sce_sys/Icon0.png
	cp $< $@

sce_sys/pic1.png: ../sce_sys/pic1.png
	cp $< $@

sce_sys/about/right.sprx:
	cp $(TOOLCHAIN)/samples/piglet/sce_sys/about/right.sprx $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '01.00'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'

eboot.bin: $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/ps4hddtools_app.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/ps4hddtools_app.elf -out=$(INTDIR)/ps4hddtools_app.oelf --eboot "eboot.bin" --paid 0x3800000000000010

$(INTDIR)/%.o: $(PROJDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $<

$(INTDIR)/%.o: $(PROJDIR)/%.cpp
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(CONTENT_ID).pkg pkg.gp4 eboot.bin build sce_sys sce_module assets/misc/ps4hddtools.elf assets/misc/lang_*.json assets/misc/languages.json

# Собирает службу в корне проекта (нужен PS4_PAYLOAD_SDK), копирует её в ../release/ps4hddtools.elf
# (руками так легко забыть - именно это и было причиной рассинхрона версий), затем чисто
# пересобирает сам PKG. Используйте это вместо простого "make", когда меняли код/версию.
.PHONY: full
full:
	$(MAKE) -C ..
	cp ../build/ps4hddtools.elf ../release/ps4hddtools.elf
	$(MAKE) clean
	$(MAKE) all

.PHONY: all clean
