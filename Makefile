TARGET = PorteroCarrera
OBJS = main.o

BUILD_PRX = 0
EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Portero Carrera

CFLAGS = -O2 -G0 -Wall -std=gnu99
LIBS = -lpspgu -lpspdisplay -lpspctrl -lpspkernel -lpspiofilemgr -lm

PSPSDK = $(shell psp-config --pspsdk-path)

include $(PSPSDK)/lib/build.mak
