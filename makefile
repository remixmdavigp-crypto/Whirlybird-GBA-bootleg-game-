#-------------------------------------------------------------------------------
# GBA Makefile - Fixed Execution Order (objcopy before gbafix)
#-------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment.")
endif

include $(DEVKITARM)/gba_rules

TARGET   := whirlybird
BUILD    := build
SOURCES  := src
AUDIO    := audio
GFX      := gfx

# GBA Flags
ARCH     := -mthumb -mthumb-interwork
CFLAGS   := -g -Wall -O2 -mcpu=arm7tdmi -mtune=arm7tdmi $(ARCH) -ffast-math
CXXFLAGS := $(CFLAGS)
LDFLAGS  := -specs=gba.specs $(ARCH) -Wl,-Map,$(TARGET).map

# Source File Inclusions
CFILES   := $(wildcard $(SOURCES)/*.c)
SFILES   := $(wildcard $(SOURCES)/*.s)
WAVFILES := $(wildcard $(AUDIO)/*.wav)
BINFILES := $(wildcard $(GFX)/*.bin)

# Object Files Target Directory
OFILES   := $(addprefix $(BUILD)/, $(notdir $(CFILES:.c=.o) $(SFILES:.s=.o) $(WAVFILES:.wav=.o) $(BINFILES:.bin=.o)))

INCLUDE  := -Iinclude -I$(BUILD) -I$(LIBGBA)/include
LIBDIRS  := -L$(LIBGBA)/lib
LIBS     := -lgba

.PHONY: all clean

all: $(BUILD) $(TARGET).gba

$(BUILD):
	@mkdir -p $(BUILD)

# Correct Order: Generate .gba with objcopy FIRST, then fix the header with gbafix
$(TARGET).gba: $(TARGET).elf
	@echo "Converting ELF to GBA binary: $@"
	@$(OBJCOPY) -O binary $< $@
	@echo "Fixing GBA Header: $@"
	@gbafix $@ -t"$(TARGET)"

$(TARGET).elf: $(OFILES)
	@echo "Linking $@"
	@$(CC) $^ $(LDFLAGS) $(LIBDIRS) $(LIBS) -o $@

# C Compilation
$(BUILD)/%.o: $(SOURCES)/%.c
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# Assembly Compilation
$(BUILD)/%.o: $(SOURCES)/%.s
	@echo "Assembling $<"
	@$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# WAV Audio Conversion
$(BUILD)/%.o: $(AUDIO)/%.wav
	@echo "Converting audio $<"
	@(bin2s $< && printf "\n") | $(AS) $(ARCH) -o $@

# GFX Binary Conversion
$(BUILD)/%.o: $(GFX)/%.bin
	@echo "Converting gfx binary $<"
	@(bin2s $< && printf "\n") | $(AS) $(ARCH) -o $@

clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).gba $(TARGET).map