#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	GBAStationNDSStub
APP_TITLE	:=	GBAStation Drastic Stub
APP_AUTHOR	:=	beiklive
APP_VERSION	:=	0.0.1
BUILD		:=	build
SOURCES		:=	source source/hooks source/lsfg \
			third_party/lsfg-vk/lsfg-vk-common/src/helpers \
			third_party/lsfg-vk/lsfg-vk-common/src/vulkan \
			third_party/lsfg-vk/lsfg-vk-backend/src \
			third_party/lsfg-vk/lsfg-vk-backend/src/extraction \
			third_party/lsfg-vk/lsfg-vk-backend/src/helpers \
			third_party/lsfg-vk/lsfg-vk-backend/src/shaderchains
DATA		:=	data
INCLUDES	:=	source source/lsfg $(PORTLIBS)/include/freetype2 \
			third_party/lsfg-vk/lsfg-vk-common/include \
			third_party/lsfg-vk/lsfg-vk-backend/include \
			third_party/lsfg-vk/lsfg-vk-backend/src
# The 26.1.4 consumer SDK packages NVK together with its required Switch
# compatibility shims.  A different SDK can still be selected via VULKAN_SDK.
VULKAN_SDK ?= $(abspath $(TOPDIR)/../switchVK/nvk-switch-26.1.4)
DFX_GENERATED ?=
ifneq ($(strip $(DFX_GENERATED)),)
DATA		+=	$(DFX_GENERATED)/data
INCLUDES	+=	$(DFX_GENERATED)/include
SOURCES		+=	$(DFX_GENERATED)/src
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

# NVK Vulkan host. DraStic's GLES texture uploads are captured and presented
# through Vulkan, so switch-mesa EGL/GLES must not be linked into this build.
DEFINES	+=	-DUSE_VULKAN -DVK_USE_PLATFORM_VI_NN

CFLAGS	:=	-Wall -Wextra $(OPTIMIZATION) -DNDEBUG -ffunction-sections -fdata-sections \
			-fno-ident -ffile-prefix-map=$(CURDIR)=. \
			-fmacro-prefix-map=$(CURDIR)=. $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE)
CXXFLAGS	:= $(CFLAGS) -Wno-missing-field-initializers -std=gnu++20

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) $(OPTIMIZATION) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=sha1 -Wl,--allow-multiple-definition

# switchVK exports a self-contained libvulkan.a. It owns Mesa/NVK and its
# generated Vulkan runtime, and must remain mutually exclusive with Mesa EGL.
LIBDIRS	:= $(VULKAN_SDK) $(PORTLIBS) $(LIBNX)
LIBS	:= -Wl,--whole-archive -lvulkan -Wl,--no-whole-archive \
		-Wl,--wrap=vk_icdGetInstanceProcAddr \
		-Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=stat -Wl,--wrap=lstat \
		-Wl,--wrap=fstat \
		-lminizip -lfreetype -lharfbuzz -lpng16 -lbz2 \
		-lz -lzstd -lnx -lstdc++ -lm

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
