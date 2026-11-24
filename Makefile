TARGET = gh101
OBJS   = src/main.o src/settings.o src/config.o src/gfx.o src/audio.o src/library.o

# JPEG wallpapers need libjpeg (part of pspdev's psp-packages).
# If your toolchain has no libjpeg, build with: make JPEG=0  (PNG still works)
JPEG ?= 1

INCDIR = src
CFLAGS = -O2 -G0 -Wall -ffast-math
ifeq ($(JPEG),1)
CFLAGS += -DHAVE_JPEG
JPEGLIB = -ljpeg
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

# libpspsystemctrl_user.a from the ARK-4 SDK (libs/SystemCtrlForUser) goes into ./ark
LIBDIR = ./ark
LIBS   = -lpspsystemctrl_user -lpng $(JPEGLIB) -lz -lpspgu -lpspaudiolib -lpspaudio \
         -lpsppower -lpsprtc -lm

PSP_FW_VERSION = 660
BUILD_PRX = 1

EXTRA_TARGETS   = EBOOT.PBP
PSP_EBOOT_TITLE = GH-1.01

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
