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
APP_FILES = mp_core.c mp_player.c mp_midi.c mp_draw.c mp_font.c mp_alloc.c mp_gme.c
GME = ../src/gme
GME_CPP = $(notdir $(wildcard $(GME)/*.cpp))
GME_OBJECTS = $(addprefix $(BUILD_DIR)/g_,$(GME_CPP:.cpp=.o)) $(BUILD_DIR)/g_emu2413.o
EXTRA_OBJECTS = $(BUILD_DIR)/a_mp_pcm.o $(BUILD_DIR)/a_miniz_tinfl.o $(BUILD_DIR)/a_mp_cxx.o

SAN ?= 0
OPT ?= -O2 -g
ifeq ($(SAN),1)
OPT += -fsanitize=address,undefined -fno-omit-frame-pointer -DMP_HOST_SYSTEM_MALLOC
LDFLAGS += -fsanitize=address,undefined
endif

DEFS = -DMP_HOST -DGME_GNW -DGME_DISABLE_STEREO_DEPTH=1 -DVGM_YM2612_GENS -DBLARGG_LITTLE_ENDIAN=1 -DLIBXMP_GNW -DLIBXMP_CORE_PLAYER -DLIBXMP_STATIC
INCLUDES = -I$(APP) -I$(XMP)/include -I$(XMP)/src -I$(MP3) -I../src/midi -I../src/codecs -I$(GME)
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

# the codecs and Game_Music_Emu on the player's heap
$(BUILD_DIR)/a_mp_pcm.o: $(APP)/mp_pcm.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -w -include $(APP)/mp_glue.h $< -o $@

$(BUILD_DIR)/a_miniz_tinfl.o: ../src/codecs/miniz_tinfl.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -w $< -o $@

$(BUILD_DIR)/a_mp_cxx.o: $(APP)/mp_cxx.cpp | $(BUILD_DIR)
	$(CXX) -c $(OPT) -MMD $(DEFS) $(INCLUDES) -fno-exceptions -fno-rtti $< -o $@

$(BUILD_DIR)/g_%.o: $(GME)/%.cpp | $(BUILD_DIR)
	$(CXX) -c $(OPT) -MMD $(DEFS) $(INCLUDES) -fno-exceptions -fno-rtti -fwrapv -w -include $(APP)/mp_glue.h $< -o $@

$(BUILD_DIR)/g_emu2413.o: $(GME)/ext/emu2413.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -w -include $(APP)/mp_glue.h $< -o $@

$(BUILD_DIR)/h_mp_host.o: mp_host.c | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(TARGET): $(XMP_OBJECTS) $(APP_OBJECTS) $(EXTRA_OBJECTS) $(GME_OBJECTS) $(BUILD_DIR)/h_mp_host.o
	$(CXX) $^ $(LDFLAGS) -lm -o $@

-include $(wildcard $(BUILD_DIR)/*.d)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
