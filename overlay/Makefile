ifeq ($(strip $(DEVKITPRO)),)
$(error Please set DEVKITPRO before building the Ultrahand overlay)
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := SwitchInfoNX
BUILD := build
APP_TITLE := SwitchInfoNX Network
APP_AUTHOR := dodosi
APP_VERSION := 0.5.0
NO_ICON := 1
export NROFLAGS := --nacp=$(TOPDIR)/$(TARGET).nacp

SOURCES := source
INCLUDES := ../include
LIBDIRS := $(PORTLIBS) $(LIBNX)
include $(TOPDIR)/../libs/libultrahand/ultrahand.mk

ARCH := -march=armv8-a+simd+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Os -ffunction-sections -fdata-sections \
	-fomit-frame-pointer -fuse-linker-plugin -finline-small-functions \
	-fno-strict-aliasing -frename-registers -falign-functions=16 \
	$(ARCH) -D__SWITCH__ -DUI_OVERRIDE_PATH=\"/config/switchinfonx_network/\"
CFLAGS += $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir))
CFLAGS += -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include
CXXFLAGS := $(CFLAGS) -std=gnu++23 -fno-unwind-tables \
	-fno-asynchronous-unwind-tables -ffast-math -Wno-dangling-else
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) \
	-Wl,--gc-sections -Wl,--as-needed
LIBS := -lcurl -lz -lminizip -lnx

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) \
	$(TOPDIR)/../source $(foreach dir,$(DATA),$(TOPDIR)/$(dir))
export DEPSDIR := $(TOPDIR)/$(BUILD)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(TOPDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CFILES += network_counters.c
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
OFILES += $(SFILES:.s=.o)
DEPENDS := $(OFILES:.o=.d)
export LD := $(CXX)
export OFILES
export LIBPATHS

.PHONY: all clean $(BUILD)
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).ovl $(TARGET).nro $(TARGET).nacp $(TARGET).elf \
		$(TARGET).lst

else
.PHONY: all
all: $(OUTPUT).ovl

$(OUTPUT).elf: $(OFILES)

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@printf 'ULTR' >> $@
	@echo built ... $(notdir $@)

-include $(DEPENDS)
endif
