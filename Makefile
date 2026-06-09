TARGET		:=	SwitchInfoNX
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=	include

TITLE		:=	Switch Info NX
AUTHOR		:=	dodosi
VERSION		:=	0.0.1

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

CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS		:=	-g $(ARCH)

LDFLAGS		:=	-specs="$(DEVKITPRO)/libnx/switch.specs" $(ARCH)
LDFLAGS		+=	-Wl,-Map,$(TARGET).map -Wl,--gc-sections

LIBS		:=	-lSDL2_ttf -lSDL2_gfx -lSDL2 -lharfbuzz -lfreetype -lbz2 -lpng -lz -lEGL -lstdc++ -lglapi -ldrm_nouveau -lnx -lm

LIBDIRS		:=	-L"$(DEVKITPRO)/libnx/lib" -L"$(DEVKITPRO)/portlibs/switch/lib"

CFILES		:=	$(wildcard $(SOURCES)/*.c)
CPPFILES	:=	$(wildcard $(SOURCES)/*.cpp)
SFILES		:=	$(wildcard $(SOURCES)/*.s)

OBJS		:=	$(CFILES:$(SOURCES)/%.c=$(BUILD)/%.o)
OBJS		+=	$(CPPFILES:$(SOURCES)/%.cpp=$(BUILD)/%.o)
OBJS		+=	$(SFILES:$(SOURCES)/%.s=$(BUILD)/%.o)

NACP		:=	$(TARGET).nacp

ifeq ($(strip $(CPPFILES)),)
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

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	$(CC) -c $(CFLAGS) -o "$@" "$<"

$(BUILD)/%.o: $(SOURCES)/%.cpp | $(BUILD)
	$(CXX) -c $(CXXFLAGS) -o "$@" "$<"

$(BUILD)/%.o: $(SOURCES)/%.s | $(BUILD)
	$(AS) -c $(ASFLAGS) -o "$@" "$<"

$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) $(OBJS) $(LIBDIRS) $(LIBS) -o "$@"

$(NACP):
	$(NACPTOOL) --create "$(TITLE)" "$(AUTHOR)" "$(VERSION)" "$@"

$(TARGET).nro: $(TARGET).elf $(NACP) $(ICON)
	$(ELF2NRO) "$<" "$@" --nacp="$(NACP)" --icon="$(ICON)"

clean:
	rm -rf "$(BUILD)" "$(TARGET).elf" "$(TARGET).nro" "$(TARGET).nacp"
