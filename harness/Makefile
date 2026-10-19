# Music Player — headless Linux harness
#
#   make            → build/mp_host
#   make SAN=1      → AddressSanitizer + UBSan, system malloc
#
#   ./build/mp_host /path/to/music -t 20000 -e 1000 -o /tmp/mp -w /tmp/mp/a.wav

BUILD_DIR = build
TARGET = $(BUILD_DIR)/mp_host

XMP = ../src/xmp
APP = ../src/app
MP3 = ../src/mp3

include ../src/xmp/sources.mk

XMP_SOURCES = $(addprefix $(XMP)/,$(XMP_FILES))
APP_FILES = mp_core.c mp_player.c mp_midi.c mp_draw.c mp_font.c mp_alloc.c

SAN ?= 0
OPT ?= -O2 -g
ifeq ($(SAN),1)
OPT += -fsanitize=address,undefined -fno-omit-frame-pointer -DMP_HOST_SYSTEM_MALLOC
LDFLAGS += -fsanitize=address,undefined
endif

DEFS = -DMP_HOST -DLIBXMP_GNW -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC
INCLUDES = -I$(APP) -I$(XMP)/include -I$(XMP)/src -I$(MP3) -I../src/midi
WARN = -Wall -Wno-unused-function -Wno-sign-compare
CFLAGS = $(OPT) -MMD -std=gnu11 $(DEFS) $(WARN) $(INCLUDES)

XMP_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/x_%.o,$(subst /,_,$(XMP_FILES)))
APP_OBJECTS = $(addprefix $(BUILD_DIR)/a_,$(APP_FILES:.c=.o))

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

define xmp_rule
$(BUILD_DIR)/x_$(subst /,_,$(1:.c=.o)): $(XMP)/$(1) | $(BUILD_DIR)
	$$(CC) -c $$(CFLAGS) -w -include $(APP)/mp_glue.h $$< -o $$@
endef
$(foreach f,$(XMP_FILES),$(eval $(call xmp_rule,$(f))))

$(BUILD_DIR)/a_%.o: $(APP)/%.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/h_mp_host.o: mp_host.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(TARGET): $(XMP_OBJECTS) $(APP_OBJECTS) $(BUILD_DIR)/h_mp_host.o
	$(CC) $^ $(LDFLAGS) -lm -o $@

-include $(wildcard $(BUILD_DIR)/*.d)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
