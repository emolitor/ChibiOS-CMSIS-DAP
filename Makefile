# Copyright (C) 2026 Eric Molitor <github.com/emolitor>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, see <https://www.gnu.org/licenses/>.

##############################################################################
# Build global options
#

ifeq ($(USE_OPT),)
  USE_OPT = -O2 -ggdb -fomit-frame-pointer -falign-functions=16
endif

ifeq ($(USE_COPT),)
  USE_COPT =
endif

ifeq ($(USE_CPPOPT),)
  USE_CPPOPT = -fno-rtti
endif

ifeq ($(USE_LINK_GC),)
  USE_LINK_GC = yes
endif

ifeq ($(USE_LDOPT),)
  USE_LDOPT =
endif

ifeq ($(USE_LTO),)
  USE_LTO = yes
endif

ifeq ($(USE_VERBOSE_COMPILE),)
  USE_VERBOSE_COMPILE = no
endif

ifeq ($(USE_SMART_BUILD),)
  USE_SMART_BUILD = yes
endif

#
# Build global options
##############################################################################

##############################################################################
# Target selection
#

# When TARGET is not specified, build all targets.
ifndef TARGET
.PHONY: all clean
all clean:
	$(MAKE) TARGET=rp2040 $@
	$(MAKE) TARGET=rp2350 $@
else

ifeq ($(TARGET),rp2040)
  MCU  = cortex-m0plus
  ifeq ($(USE_FPU),)
    USE_FPU = no
  endif
  TARGET_DEFS = -DTARGET_RP2040
else ifeq ($(TARGET),rp2350)
  MCU  = cortex-m33
  ifeq ($(USE_FPU),)
    USE_FPU = softfp
  endif
  ifeq ($(USE_FPU_OPT),)
    USE_FPU_OPT = -mfloat-abi=$(USE_FPU) -mfpu=fpv5-sp-d16
  endif
  TARGET_DEFS = -DTARGET_RP2350
else
  $(error Unknown TARGET=$(TARGET). Use rp2040 or rp2350)
endif

#
# Target selection
##############################################################################

##############################################################################
# Project, target, sources and paths
#

PROJECT = ch

# ChibiOS location — default to ./ChibiOS checked out via 'make chibios'.
CHIBIOS  ?= ./ChibiOS
CONFDIR  := ./cfg
BUILDDIR := ./build/$(TARGET)
DEPDIR   := ./.dep

PICOTOOL ?= picotool
CHIBIOS_SVN ?= svn://svn.code.sf.net/p/chibios/code/trunk

# Guard: ChibiOS must exist for build targets.
ifeq ($(wildcard $(CHIBIOS)/os/license/license.mk),)

# Only filter if not running 'make chibios'.
ifeq ($(filter chibios,$(MAKECMDGOALS)),)
$(warning ChibiOS not found at $(CHIBIOS). Run 'make chibios' first or set CHIBIOS=)
endif

all:
	@echo "Error: ChibiOS not found at $(CHIBIOS)"
	@echo "Run 'make chibios' to checkout from SVN, or set CHIBIOS=/path/to/chibios"
	@exit 1

else

# Licensing files.
include $(CHIBIOS)/os/license/license.mk
# Target-specific startup, platform, board, port, and linker script.
ifeq ($(TARGET),rp2040)
include $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk/startup_rp2040.mk
include $(CHIBIOS)/os/hal/ports/RP/RP2040/platform.mk
include $(CHIBIOS)/os/hal/boards/RP_PICO_RP2040/board.mk
include $(CHIBIOS)/os/common/ports/ARMv6-M/compilers/GCC/mk/port_rp2.mk
LDSCRIPT = $(STARTUPLD)/RP2040_FLASH.ld
else ifeq ($(TARGET),rp2350)
include $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk/startup_rp2350.mk
include $(CHIBIOS)/os/hal/ports/RP/RP2350/platform.mk
include $(CHIBIOS)/os/hal/boards/RP_PICO2_RP2350/board.mk
include $(CHIBIOS)/os/common/ports/ARMv8-M-ML/compilers/GCC/mk/port_rp2.mk
LDSCRIPT = $(STARTUPLD)/RP2350_FLASH.ld
endif
# HAL-OSAL files.
include $(CHIBIOS)/os/hal/hal.mk
include $(CHIBIOS)/os/hal/osal/rt-nil/osal.mk
# RTOS files.
include $(CHIBIOS)/os/rt/rt.mk

CSRC = $(ALLCSRC) \
       main.c \
       usbcfg.c \
       dap.c \
       swd.c

CPPSRC = $(ALLCPPSRC)

ASMSRC = $(ALLASMSRC)

ASMXSRC = $(ALLXASMSRC)

INCDIR = $(CONFDIR) $(ALLINC)

CWARN = -Wall -Wextra -Wundef -Wstrict-prototypes

CPPWARN = -Wall -Wextra -Wundef

#
# Project, target, sources and paths
##############################################################################

##############################################################################
# User section
#

UDEFS = -DCRT0_VTOR_INIT=1 -DCRT0_EXTRA_CORES_NUMBER=1 $(TARGET_DEFS)

UADEFS = -DCRT0_VTOR_INIT=1 -DCRT0_EXTRA_CORES_NUMBER=1 $(TARGET_DEFS)

UINCDIR =

ULIBDIR =

ULIBS =

#
# User section
##############################################################################

##############################################################################
# Rules
#

RULESPATH = $(CHIBIOS)/os/common/startup/ARMCMx/compilers/GCC/mk
include $(RULESPATH)/arm-none-eabi.mk
include $(RULESPATH)/rules.mk

#
# Rules
##############################################################################

##############################################################################
# Custom rules — build
#

$(BUILDDIR)/$(PROJECT).uf2: $(BUILDDIR)/$(PROJECT).elf
	$(PICOTOOL) uf2 convert $(BUILDDIR)/$(PROJECT).elf $(BUILDDIR)/$(PROJECT).uf2

# End of ChibiOS guard.
endif

#
# Custom rules — build
##############################################################################

# End of TARGET guard.
endif

##############################################################################
# PIO assembly (requires pioasm in PATH)
#

# Regenerate PIO header from assembly source.
# The generated header is checked in; this target is only needed when
# modifying probe_swd.pio.
.PHONY: pioasm
pioasm: probe_swd.pio
	pioasm $< probe_swd.pio.h

#
# PIO assembly
##############################################################################

##############################################################################
# ChibiOS checkout (works without ChibiOS present)
#

.PHONY: chibios

# Checkout/update ChibiOS from SourceForge SVN.
# Usage:
#   make chibios                      - checkout trunk
#   make chibios CHIBIOS_REV=17755    - checkout specific revision
chibios:
ifeq ($(wildcard $(CHIBIOS)/.svn),)
ifdef CHIBIOS_REV
	svn checkout -r $(CHIBIOS_REV) $(CHIBIOS_SVN) $(CHIBIOS)
else
	svn checkout $(CHIBIOS_SVN) $(CHIBIOS)
endif
else
ifdef CHIBIOS_REV
	svn update -r $(CHIBIOS_REV) $(CHIBIOS)
else
	svn update $(CHIBIOS)
endif
endif

#
# ChibiOS checkout
##############################################################################
