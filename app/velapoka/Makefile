############################################################################
# contest2026_284_zhuanqianmaibanzi/app/velapoka/Makefile
#
# SPDX-License-Identifier: Apache-2.0
############################################################################

include $(APPDIR)/Make.defs

PROGNAME = velapoka
PRIORITY = $(CONFIG_EXAMPLES_VELAPOKA_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_VELAPOKA_STACKSIZE)
MODULE = $(CONFIG_EXAMPLES_VELAPOKA)

MAINSRC = velapoka_main.c
CSRCS = velapoka_camera.c velapoka_inspect.c velapoka_state.c \
	velapoka_storage.c velapoka_network.c velapoka_ui.c

include $(APPDIR)/Application.mk
