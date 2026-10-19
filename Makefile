# Music Player for Game & Watch Retro-Go SD — GWHB homebrew.
#
#   make                  — build + pack → MusicPlayer.bin
#   make docker           — same build inside the Retro-Go SD builder image
#   make harness          — headless Linux test harness (harness/mp_host.c)
#
# SD card (Retro-Go SD v2.0.0-rc1 or later):
#   /homebrews/MusicPlayer.bin (this homebrew)
#   /music/...                 (the music: MOD, S3M, XM, IT, MP3)
#
# Verbose compiler lines: make V=

#######################################
# Project identity
#######################################
PROJECT_KIND := homebrew

CORE_NAME  := mp
CORE_ENTRY := app_main

HB_NAME    := Music Player
PACKED_BIN := MusicPlayer.bin

XMP_DIR := src/xmp
APP_DIR := src/app
MP3_DIR := src/mp3

include $(XMP_DIR)/sources.mk

# libxmp-lite on the player's heap (src/app/mp_glue.h)
XMP_C_SOURCES := $(addprefix $(XMP_DIR)/,$(XMP_FILES))

CORE_C_SOURCES := $(XMP_C_SOURCES) \
$(APP_DIR)/mp_core.c \
$(APP_DIR)/mp_player.c \
$(APP_DIR)/mp_draw.c \
$(APP_DIR)/mp_font.c \
$(APP_DIR)/mp_alloc.c \
$(APP_DIR)/mp_libc.c \
$(APP_DIR)/mp_main.c

CORE_C_INCLUDES := -I$(APP_DIR) -I$(XMP_DIR)/include -I$(XMP_DIR)/src -I$(MP3_DIR)

CORE_C_DEFS := -DPROJECT_KIND_HOMEBREW=1 -DLIBXMP_GNW -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC

OPT := -O2

CORE_LDLIBS := -lgcc

GNW_CORE_SDK ?= sdk
BUILD_DIR ?= build/$(PROJECT_KIND)

include $(GNW_CORE_SDK)/Makefile

#######################################
# libxmp specific flags
#######################################
XMP_OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(XMP_C_SOURCES:.c=.o)))
XMP_FLAGS := -include $(APP_DIR)/mp_glue.h -std=gnu11 -w
$(XMP_OBJECTS): CFLAGS += $(XMP_FLAGS)
$(C_OBJECTS): Makefile

#######################################
# Packed header version
#######################################
CORE_VERSION ?= $(shell git describe --tags --dirty 2>/dev/null || echo NOTAG)

#######################################
# Pack (GWHB)
#######################################
PACK_HOMEBREW := $(GNW_CORE_SDK)/tools/pack_homebrew.py
GEN_COVER     := scripts/gen_homebrew_cover.py
COVER_JPG     := $(BUILD_DIR)/cover.jpg
COVER_WIDTH   ?= 128
COVER_HEIGHT  ?= 96

.PHONY: pack cover
cover: $(COVER_JPG)

# Must stay <= 186x100 and <= 10 KiB (firmware cover cache).
$(COVER_JPG): $(GEN_COVER) | $(BUILD_DIR)
	$(V)$(ECHO) "[ COVER ] $(COVER_JPG) $(COVER_WIDTH)x$(COVER_HEIGHT)"
	$(V)python3 $(GEN_COVER) \
		--out $(COVER_JPG) \
		--title "$(HB_NAME)" \
		--width $(COVER_WIDTH) \
		--height $(COVER_HEIGHT)

pack: $(TARGET_BIN) $(COVER_JPG)
	$(V)$(ECHO) [ PACK GWHB ] $(PACKED_BIN) version=$(CORE_VERSION)
	$(V)python3 $(PACK_HOMEBREW) \
		--elf $(TARGET_ELF) --bin $(TARGET_BIN) \
		--name "$(HB_NAME)" --version "$(CORE_VERSION)" \
		--cover $(COVER_JPG) \
		--out $(PACKED_BIN)

all: pack

clean::
	$(V)rm -f $(PACKED_BIN) $(COVER_JPG)

# Read-only helpers for CI / scripts (make print-PACKED_BIN, etc.).
.PHONY: print-PROJECT_KIND print-PACKED_BIN print-CORE_NAME print-DOCKER_IMAGE \
	print-TARGET_ELF print-TARGET_MAP print-CORE_VERSION
print-PROJECT_KIND:
	@echo $(PROJECT_KIND)
print-PACKED_BIN:
	@echo $(PACKED_BIN)
print-CORE_NAME:
	@echo $(CORE_NAME)
print-DOCKER_IMAGE:
	@echo $(DOCKER_IMAGE)
print-TARGET_ELF:
	@echo $(TARGET_ELF)
print-TARGET_MAP:
	@echo $(BUILD_DIR)/$(CORE_NAME)_core.map
print-CORE_VERSION:
	@echo $(CORE_VERSION)

#######################################
# Headless Linux harness (same player, virtual clock)
#######################################
.PHONY: harness
harness:
	$(MAKE) -C harness

#######################################
# Docker (same image as the firmware repo)
#######################################
.PHONY: docker docker_pull docker_shell

RELEASE_VERSION ?= v1.5
DOCKER_REPOSITORY ?= sylverb/retro-go-sd-builder
DOCKER_IMAGE ?= $(DOCKER_REPOSITORY):$(RELEASE_VERSION)

DOCKER_TTY_FLAG := $(shell if [ -t 0 ]; then echo -it; else echo; fi)
DOCKER_USER := $(shell id -u):$(shell id -g)
DOCKER_RUN := docker run --rm $(DOCKER_TTY_FLAG) \
	--user $(DOCKER_USER) \
	-v "$(CURDIR):/opt/workdir" \
	-w /opt/workdir \
	$(DOCKER_IMAGE)

docker:
	$(V)$(ECHO) "[ DOCKER ]" $(DOCKER_IMAGE)
	$(V)$(DOCKER_RUN) make --no-print-directory -j$$(nproc)

docker_pull:
	$(V)docker pull $(DOCKER_IMAGE)

docker_shell:
	$(DOCKER_RUN) bash
