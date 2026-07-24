CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wpedantic

HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
LIBOOT_LIBRARY := ../dist/liboot.dylib
RPATH_FLAG     := -Wl,-rpath,@loader_path/../dist
else
LIBOOT_LIBRARY := ../dist/liboot.so
RPATH_FLAG     := -Wl,-rpath,'$$ORIGIN/../dist'
endif

all: engine basic

engine: engine.c $(LIBOOT_LIBRARY) ../dist/include/liboot_engine.h
	$(CC) $(CFLAGS) -I../dist/include engine.c -o engine -L../dist -loot \
		$(RPATH_FLAG)

basic: basic.c $(LIBOOT_LIBRARY) ../dist/include/liboot.h
	$(CC) $(CFLAGS) -I../dist/include basic.c -o basic -L../dist -loot \
		$(RPATH_FLAG)

$(LIBOOT_LIBRARY) ../dist/include/liboot.h ../dist/include/liboot_engine.h:
	$(MAKE) -C .. lib

clean:
	rm -f engine basic

.PHONY: all clean
