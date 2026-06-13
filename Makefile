TARGET		:=	SwitchInfoNX
BUILD		:=	build
INCLUDES	:=	include include/mtp

TITLE		:=	Switch Info NX
AUTHOR		:=	dodosi
VERSION		:=	0.0.3

ICON		:=	icon.jpg

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

CC		:=	"$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-gcc"
CXX		:=	"$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-g++"
AS		:=	"$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-as"
ELF2NRO		:=	"$(DEVKITPRO)/tools/bin/elf2nro"
NACPTOOL	:=	"$(DEVKITPRO)/tools/bin/nacptool"

ARCH		:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS		:=	-g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES) -D__SWITCH__
CFLAGS		+=	$(foreach dir,$(INCLUDES),-I"$(CURDIR)/$(dir)")
CFLAGS		+=	-I"$(DEVKITPRO)/libnx/include" -I"$(DEVKITPRO)/portlibs/switch/include"

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -std=gnu++17

ASFLAGS		:=	-g $(ARCH)

LDFLAGS		:=	-specs="$(DEVKITPRO)/libnx/switch.specs" $(ARCH)
LDFLAGS		+=	-Wl,-Map,$(TARGET).map -Wl,--gc-sections

LIBS		:=	-lSDL2_ttf -lSDL2_gfx -lSDL2 -lharfbuzz -lfreetype -lbz2 -lpng -lz -lEGL -lstdc++ -lstdc++fs -lglapi -ldrm_nouveau -lnx -lm

LIBDIRS		:=	-L"$(DEVKITPRO)/libnx/lib" -L"$(DEVKITPRO)/portlibs/switch/lib"

SRC_C		:=	$(wildcard source/*.c)
SRC_CPP		:=	$(wildcard source/*.cpp)
MTP_C		:=	$(wildcard source/mtp/*.c)
MTP_CPP		:=	$(wildcard source/mtp/*.cpp)
SRC_S		:=	$(wildcard source/*.s) $(wildcard source/mtp/*.s)

OBJ_C		:=	$(SRC_C:source/%.c=$(BUILD)/%.o)
OBJ_CPP		:=	$(SRC_CPP:source/%.cpp=$(BUILD)/%.o)
OBJ_MTP_C	:=	$(MTP_C:source/mtp/%.c=$(BUILD)/mtp_%.o)
OBJ_MTP_CPP	:=	$(MTP_CPP:source/mtp/%.cpp=$(BUILD)/mtp_%.o)
OBJ_S		:=	$(SRC_S:source/%.s=$(BUILD)/%.o) $(SRC_S:source/mtp/%.s=$(BUILD)/mtp_%.o)

OBJS		:=	$(OBJ_C) $(OBJ_CPP) $(OBJ_MTP_C) $(OBJ_MTP_CPP) $(OBJ_S)

NACP		:=	$(TARGET).nacp

ifeq ($(strip $(SRC_CPP)$(MTP_CPP)),)
LD	:=	$(CC)
else
LD	:=	$(CXX)
endif

.PHONY: all clean icon

all: $(TARGET).nro

icon:
	@bash tools/make_icon.sh

$(BUILD):
	mkdir -p "$@"

$(BUILD)/%.o: source/%.c | $(BUILD)
	$(CC) -c $(CFLAGS) -o "$@" "$<"

$(BUILD)/%.o: source/%.cpp | $(BUILD)
	$(CXX) -c $(CXXFLAGS) -o "$@" "$<"

$(BUILD)/mtp_%.o: source/mtp/%.c | $(BUILD)
	$(CC) -c $(CFLAGS) -o "$@" "$<"

$(BUILD)/mtp_%.o: source/mtp/%.cpp | $(BUILD)
	$(CXX) -c $(CXXFLAGS) -o "$@" "$<"

$(BUILD)/%.o: source/%.s | $(BUILD)
	$(AS) -c $(ASFLAGS) -o "$@" "$<"

$(BUILD)/mtp_%.o: source/mtp/%.s | $(BUILD)
	$(AS) -c $(ASFLAGS) -o "$@" "$<"

$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBDIRS) $(LIBS) -o "$@"

$(NACP):
	$(NACPTOOL) --create "$(TITLE)" "$(AUTHOR)" "$(VERSION)" "$@"

$(TARGET).nro: $(TARGET).elf $(NACP) $(ICON)
	$(ELF2NRO) "$<" "$@" --nacp="$(NACP)" --icon="$(ICON)"

clean:
	rm -rf "$(BUILD)" "$(TARGET).elf" "$(TARGET).nro" "$(TARGET).nacp"
