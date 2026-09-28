#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
TARGET		:=	booru3ds
APP_TITLE	:=	Booru3DS
APP_AUTHOR	:=	MisakiP_ / Misakii-P
APP_DESCRIPTION	:=	SFW Booru API image board explorer for 3DS.
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
ROMFS		:=	romfs

#---------------------------------------------------------------------------------
# CIA packaging (make cia)
#
# TITLEID is the 3DS Title ID, the 8-byte value the console uses to tell this
# app apart from every other installed title. It was randomised once so a build
# here cannot collide with someone else's, and it MUST stay the same for every
# rebuild - a changed value makes the console install a second copy instead of
# updating the first. Only change it if you mean to ship a new app.
TITLEID		?=	000400000E2B9700

# Optional 3DS banner frames (banner/*.png, see the bannertool layout).
# BANNER picks it up when present; without it the CIA just carries the icon.
BANNERDIR	:=	banner

# makerom packs the CIA. It is not part of devkitPro and 3dstool/makerom were
# both pulled from distribution in 2018, so build it from the still-maintained
# 3DSGuy/Project_CTR and put bin/makerom on PATH:
#   git clone --depth 1 https://github.com/3DSGuy/Project_CTR.git
#   cd Project_CTR/makerom && make deps && make && sudo install -m755 bin/makerom /usr/local/bin/
# Optionally, to get a 3DS HOME-menu banner instead of just the icon,
# install bannertool (https://github.com/SteveTev/bannertool) too - the
# `cia` target uses $(BANNER) if it exists and the SMDH icon if not.
RSF		:=	$(CURDIR)/cia-template.rsf

# 3DS HOME-menu banner. assets/banner.png is 256x128 and assets/banner.wav is
# the tune; bannertool turns the pair into a CFRG .bnr. Without them the cia
# target falls back to passing the SMDH for both the icon and the banner
# chunk, which is the usual homebrew fallback (icon only, no animation).
# Target loudness for the banner jingle, in LUFS (EBU R128). -10 is
# deliberately well above the -14 LUFS streaming target: this is a three
# second jingle, not programme material, and it is played through the
# console's own speaker. Override with e.g. `make cia BANNER_LUFS=-8`.
BANNER_LUFS	?=	-10
BANNER_TP	?=	-1.0
BANNER_PNG	:=	$(CURDIR)/assets/banner.png
BANNER_WAV	:=	$(CURDIR)/assets/banner.wav
BANNER		:=	$(BUILD)/$(TARGET).bnr
# a real .bnr gives the HOME menu its animation; without one the SMDH is used
# for both chunks, which is what every other homebrew CIA does
BANNERFILE	=	$(if $(wildcard $(BANNER)),$(CURDIR)/$(BANNER),$(OUTPUT).smdh)

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lturbojpeg -lctru -lm

LIBDIRS	:= $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))

export LD	:=	$(CC)

export T3XFILES :=  $(GFXFILES:.t3s=.t3x)

export OFILES_BIN	:=	$(addsuffix .o,$(T3XFILES))
export OFILES := $(OFILES_BIN) $(CFILES:.c=.o)

export HFILES	:=	$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(OUTPUT).smdh

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
ifneq ($(ROMFS),)
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all banner banner-help cia cia-tools clean

#---------------------------------------------------------------------------------
.PHONY: all banner banner-help cia cia-tools clean

all: $(BUILD) $(T3XFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
# CIA: an installable title, as opposed to the .3dsx homebrew loader.
# Needs two tools devkitPro does not ship. Check before building so a missing
# packer fails here with an explanation instead of mid-way through.
cia-tools:
#---------------------------------------------------------------------------------
	@command -v makerom >/dev/null 2>&1 || { \
		echo "ERROR: 'makerom' not found in PATH - needed to pack the CIA."; \
		echo "       It is not in devkitPro, and 3dstool/makerom were pulled"; \
		echo "       from distribution in 2018. Build it from the maintained"; \
		echo "       source instead:"; \
		echo "         git clone --depth 1 https://github.com/3DSGuy/Project_CTR.git"; \
		echo "         cd Project_CTR/makerom && make deps && make"; \
		echo "         sudo install -m755 bin/makerom /usr/local/bin/makerom"; \
		exit 1; }
	@test -f $(RSF) || { echo "ERROR: $(RSF) is missing."; exit 1; }
	@uid=`sed -n 's/^[[:space:]]*UniqueId[[:space:]]*:[[:space:]]*0x\([0-9A-Fa-f]*\).*/\1/p' $(RSF) | head -1`; \
	 test -n "$$uid" || { echo "ERROR: no TitleInfo/UniqueId in $(RSF)"; exit 1; }; \
	 want=`printf '%016X' $$(( 0x0004 * 0x1000000000000 + 0x$$uid * 256 ))`; \
	 if [ "$$want" != "$(TITLEID)" ]; then \
		echo "ERROR: TITLEID does not match the RSF."; \
		echo "       TITLEID (Makefile) : $(TITLEID)"; \
		echo "       RSF UniqueId      : 0x$$uid  -> Title ID $$want"; \
		echo "       makerom builds the real 64-bit id as"; \
		echo "       (0x0004<<48)|(category<<32)|(UniqueId<<8)|variation, so"; \
		echo "       only the low 24 bits of UniqueId are real entropy. If these"; \
		echo "       drift apart the console sees a different title and will"; \
		echo "       install a second copy instead of updating."; \
		exit 1; fi
	@echo "cia tools ok (makerom)"
	@echo "TITLEID = $(TITLEID)"

banner: $(BANNER)
#---------------------------------------------------------------------------------
	@ls -la $(BANNER)

# a real file rule, so the cia target's $(BANNERFILE) lookup sees the result.
# bannertool only exists if it was built; without it we silently fall back to
# the SMDH rather than failing the whole build.
$(BANNER): $(BANNER_PNG) $(wildcard $(BANNER_WAV))
#---------------------------------------------------------------------------------
	@mkdir -p $(BUILD)
	@command -v bannertool >/dev/null 2>&1 || { \
		echo "bannertool not found - skipping, HOME menu will show the icon only"; \
		exit 0; }
	@# The 3DS banner has two hard limits, both learned the hard way:
	@#   - audio must be PCM 16-bit / 32kHz / stereo. 24-bit 44.1kHz input
	@#     produces a container the HOME menu drops entirely (no banner).
	@#   - audio is capped at 3 seconds. Longer and the banner appears but
	@#     the 3DS ignores the sound and falls back to its stock jingle.
	@# You cannot simply add gain here: 0 dBFS is a hard ceiling and the
	@# track already peaks near it, so turning it up only clips. What makes
	@# it sound louder is squeezing the dynamic range instead - the
	@# acompressor lifts the quiet passages, which raises how loud the
	@# whole thing reads without needing any more peak. loudnorm then
	@# sets the level, holding true peak at $(BANNER_TP) dBTP.
	@# NB this also normalises WAVE_FORMAT_EXTENSIBLE input, which
	@# bannertool's own WAV reader does not handle.
	@wav=$(BANNER_WAV); \
	 if [ -f "$$wav" ]; then \
		if command -v ffmpeg >/dev/null 2>&1; then \
			ffmpeg -y -loglevel error -i "$$wav" -t 3.0 \
				-af "acompressor=threshold=-20dB:ratio=6:attack=5:release=120:knee=8,loudnorm=I=$(BANNER_LUFS):TP=$(BANNER_TP):LRA=4" \
				-ar 32000 -ac 2 -sample_fmt s16 $(BUILD)/banner.wav; \
			wav=$(BUILD)/banner.wav; \
			echo "  normalised to 16bit/32kHz/stereo, 3.0s max, $(BANNER_LUFS) LUFS"; \
		else \
			echo "  WARNING: ffmpeg not found, using $(BANNER_WAV) as-is."; \
			echo "           The 3DS wants 16-bit/32kHz/stereo and at most 3s;"; \
			echo "           24-bit/44.1kHz drops the banner entirely, and over"; \
			echo "           3s leaves the banner with its stock jingle."; \
		fi; \
		a="-a $$wav"; \
	 else a=""; fi; \
	 bannertool makebanner -i $(BANNER_PNG) $$a -o $@

banner-help:
#---------------------------------------------------------------------------------
	@command -v bannertool >/dev/null 2>&1 || { \
		echo "bannertool not found - the CIA will show the icon only."; \
		echo "  git clone --recurse-submodules https://github.com/diasurgical/bannertool.git"; \
		echo "  cd bannertool && make"; \
		echo "  sudo install -m755 output/linux-x86_64/bannertool /usr/local/bin/"; \
		exit 0; }
	@echo "bannertool ok: $$(command -v bannertool)"

cia: cia-tools banner
#---------------------------------------------------------------------------------
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
	@echo "building CIA  TITLEID=$(TITLEID)"
	@echo "  banner: $(BANNERFILE)"
	@makerom -f cia -o $(TOPDIR)/$(TARGET).cia \
		-rsf $(RSF) -target t -ignoresign -exefslogo -ver 0 \
		-elf $(OUTPUT).elf -icon $(OUTPUT).smdh -banner $(BANNERFILE) \
		$(EXTRA_CIA_FLAGS)
	@ls -la $(TOPDIR)/$(TARGET).cia

$(BUILD):
	@mkdir -p $@

$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i gfx/$*.t3s -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf \
		$(TARGET).cia $(TARGET).nacp

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
