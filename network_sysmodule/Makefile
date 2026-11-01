ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO before building the network sysmodule")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET := SwitchInfoNXNetwork
BUILD := build
SOURCES := source
INCLUDES := ../include
CONFIG_JSON := sysmodule.json

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -O2 -ffunction-sections \
	$(ARCH) -I$(TOPDIR)/../include $(INCLUDE) -D__SWITCH__
LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH)

LIBS := -lnx
LIBDIRS := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(TOPDIR)/source $(TOPDIR)/../source
export DEPSDIR := $(CURDIR)/$(BUILD)
export LD := $(CC)
export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
export INCLUDE := -I$(TOPDIR)/../include \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	-I$(TOPDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export OFILES := main.o network_counters.o

.PHONY: all clean $(BUILD)
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $@ -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
else
.PHONY: all
all: $(OUTPUT).nsp

$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso: $(OUTPUT).elf

$(OUTPUT).elf: $(OFILES)

-include $(OFILES:.o=.d)
endif
