#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	GBAStationDrasticStub
APP_TITLE	:=	GBAStation Drastic Stub
APP_AUTHOR	:=	beiklive
APP_VERSION	:=	0.0.1
BUILD		:=	build
SOURCES		:=	source source/hooks
DATA		:=	data
INCLUDES	:=	source
DFX_GENERATED ?=
ifneq ($(strip $(DFX_GENERATED)),)
DATA		+=	$(DFX_GENERATED)/data
INCLUDES	+=	$(DFX_GENERATED)/include
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
OPTIMIZATION := -O3 -flto=auto

# __SWITCH__ for libnx; DRASTIC_NX gates the port-specific host branches.
DEFINES	:=	-D__SWITCH__ -D_GNU_SOURCE -DDRASTIC_NX -DDRASTIC_NX_VERSION='"$(APP_VERSION)"'
ifneq ($(strip $(DFX_GENERATED)),)
DEFINES	+=	-DDRASTIC_DFX_GENERATED
endif

# This project now ships one standalone OpenGL host only.
DEFINES	+=	-DUSE_OPENGL

CFLAGS	:=	-Wall -Wextra $(OPTIMIZATION) -DNDEBUG -ffunction-sections -fdata-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. \
			-fmacro-prefix-map=$(CURDIR)=. $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -Wno-missing-field-initializers

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) $(OPTIMIZATION) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=sha1

# nx supplies audren, HID, applet, and filesystem services. Drastic's OpenSL ES
# ABI is implemented directly by the audren-backed source/opensles.c layer.
# EGL/GLESv2/glapi/drm_nouveau: switch-mesa/nouveau GL.
LIBDIRS	:= $(PORTLIBS) $(LIBNX)
LIBS	:= -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lminizip -lz -lnx -lstdc++ -lm

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

absolute_or_local = $(if $(filter /%,$(1)),$(1),$(CURDIR)/$(1))
export VPATH	:=	$(foreach dir,$(SOURCES),$(call absolute_or_local,$(dir))) \
			$(foreach dir,$(DATA),$(call absolute_or_local,$(dir)))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# link with g++ so mesa's C++ EGL/GLES pulls in libstdc++
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(call absolute_or_local,$(dir))) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)


export APP_ICON := $(TOPDIR)/icon.png

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf \
		GBAStationDrasticStub.nro GBAStationDrasticStub.elf GBAStationDrasticStub.map
	@rm -f *.o

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro	:	$(OUTPUT).elf
endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
