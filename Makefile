CXX ?= g++
CXXFLAGS ?= -O2 -std=c++11 -Wall -Wextra
LIB = lib/ili9341
INC = -I$(LIB) -I$(LIB)/examples
# Luckfox Pico SDK cross compiler; override with `make deploy CROSS=...`
CROSS ?= $(HOME)/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-g++
ARMFLAGS = -O3 -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c++11 -Wall -Wextra -pthread
GAME_SRC = src/cargame.cpp src/game.h src/engine3d.h src/objload.h $(LIB)/ILI9341.cpp

all: cargame demo3d

# host build: for `./cargame ppm ...` previews and `./cargame bench`
cargame: $(GAME_SRC)
	$(CXX) $(CXXFLAGS) -pthread $(INC) -o $@ src/cargame.cpp $(LIB)/ILI9341.cpp

board/cargame: $(GAME_SRC)
	mkdir -p board
	$(CROSS) $(ARMFLAGS) $(INC) -o $@ src/cargame.cpp $(LIB)/ILI9341.cpp

deploy: board/cargame
	adb push board/cargame /root/cargame
	adb shell mkdir -p /root/assets/cars
	adb push assets/cars/. /root/assets/cars/
	adb shell sync                                # flush to flash: pulling power right after a push left a 0-byte game

# start the game at every boot, no PC needed. The ESP32 is removed, so its Wi-Fi boot script is parked in /root.
# undo: adb shell "rm /etc/init.d/S99zcargame; mv /root/S98esp32wifi.disabled /etc/init.d/S98esp32wifi"
autostart:
	adb shell "[ ! -e /etc/init.d/S98esp32wifi ] || mv /etc/init.d/S98esp32wifi /root/S98esp32wifi.disabled"
	adb push tools/S99zcargame /etc/init.d/S99zcargame
	adb shell "chmod +x /etc/init.d/S99zcargame; sync"

demo3d: src/demo3d.cpp src/engine3d.h src/objload.h
	$(CXX) $(CXXFLAGS) $(INC) -o $@ $< $(LIB)/ILI9341.cpp

test: src/test3d.cpp src/engine3d.h src/objload.h src/game.h
	$(CXX) $(CXXFLAGS) -o test3d $< && ./test3d

clean:
	rm -rf cargame demo3d test3d board
.PHONY: all deploy autostart test clean
