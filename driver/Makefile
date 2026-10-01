# Builds the InnerLoop.driver Audio Server Plug-In bundle with clang directly
# (no Xcode project needed — the AudioServerPlugIn API is plain C, and a HAL
# plugin is just a `-bundle` Mach-O plus an Info.plist in the right layout).
#
# Usage (run on macOS):
#   make            # build build/InnerLoop.driver
#   make sign       # ad-hoc code-sign it (pass SIGN_ID=<Developer ID> to use a real identity)
#   make install    # copy to /Library/Audio/Plug-Ins/HAL and restart coreaudiod
#   make uninstall  # remove it and restart coreaudiod
#   make clean

DRIVER_NAME  := InnerLoop
BUILD_DIR    := build
BUNDLE       := $(BUILD_DIR)/$(DRIVER_NAME).driver
CONTENTS_DIR := $(BUNDLE)/Contents
MACOS_DIR    := $(CONTENTS_DIR)/MacOS
BINARY       := $(MACOS_DIR)/$(DRIVER_NAME)

SRCS := src/InnerLoopDriver.c src/InnerLoopRingBuffer.c
HDRS := $(wildcard include/*.h)

CC      := clang
CFLAGS  := -Wall -Wextra -Wno-unused-parameter -O2 -std=c11 -Iinclude
LDFLAGS := -bundle -framework CoreAudio -framework CoreFoundation

INSTALL_DIR := /Library/Audio/Plug-Ins/HAL
SIGN_ID     ?= -

.PHONY: all sign install uninstall reload clean

all: $(BUNDLE)

$(BINARY): $(SRCS) $(HDRS)
	@mkdir -p $(MACOS_DIR)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

$(BUNDLE): $(BINARY) Info.plist
	@mkdir -p $(CONTENTS_DIR)
	cp Info.plist $(CONTENTS_DIR)/Info.plist
	@touch $(BUNDLE)

sign: $(BUNDLE)
	codesign --force --deep --sign "$(SIGN_ID)" $(BUNDLE)
	codesign --verify --verbose $(BUNDLE)

install: sign
	sudo rm -rf "$(INSTALL_DIR)/$(DRIVER_NAME).driver"
	sudo cp -R $(BUNDLE) "$(INSTALL_DIR)/"
	sudo chown -R root:wheel "$(INSTALL_DIR)/$(DRIVER_NAME).driver"
	$(MAKE) reload

uninstall:
	sudo rm -rf "$(INSTALL_DIR)/$(DRIVER_NAME).driver"
	$(MAKE) reload

reload:
	sudo killall coreaudiod || true

clean:
	rm -rf $(BUILD_DIR)
