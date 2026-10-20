CXX      := /opt/ps4-payload-sdk/bin/orbis-clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
TARGET   := build/ps4hddtools.elf
SOURCES  := src/main.cpp src/http_server.cpp src/disk_scan.cpp src/smart.cpp src/check.cpp src/scan.cpp
HEADERS  := src/app_state.h src/http_server.h src/disk_scan.h src/index_html.h src/smart.h src/check.h src/scan.h
all: $(TARGET)
$(TARGET): $(SOURCES) $(HEADERS) web/index.html
	mkdir -p build
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(SOURCES)
clean:
	rm -f $(TARGET) *.o
.PHONY: all clean
