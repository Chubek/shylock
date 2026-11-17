# Verified NaiveFinder/collection replay is an explicit plugin-to-plugin contract.
ifeq ($(filter dragnet,$(PLUGINS)),)
$(error Poindexter requires Dragnet for verified evidence replay; include dragnet in PLUGINS)
endif
PLUGIN_ON1X += $(wildcard plugins/poindexter/*.on1x)
PLUGIN_NATIVE += $(wildcard plugins/poindexter/*.c)
PLUGIN_ALLOCATION_NATIVE += plugins/poindexter/sheet.c
PLUGIN_HEADERS += $(wildcard plugins/poindexter/*.h)
PLUGIN_KERNELS += tui termkey immgtk fontdraw
PLUGIN_EVENTS += observation.reviewed worksheet.opened worksheet.saved

GUI ?= auto
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell pkg-config --libs sdl2 2>/dev/null)
ifeq ($(GUI),0)
SDL_CFLAGS :=
SDL_LIBS :=
endif
ifeq ($(GUI),1)
ifeq ($(strip $(SDL_LIBS)),)
$(error GUI=1 requires SDL2 development files and pkg-config)
endif
endif
PLUGIN_FLAGS += $(if $(strip $(SDL_LIBS)),-DSHY_HAVE_SDL2=1,-DSHY_HAVE_SDL2=0) $(SDL_CFLAGS) $(SDL_LIBS)
