HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
LIBOOT_LIBRARY   := ../dist/liboot.dylib
RPATH_FLAG       := -Wl,-rpath,@loader_path/../dist
OPENGL_LIBS      := -framework OpenGL
LIBRARY_PATH_ENV := DYLD_LIBRARY_PATH=../dist
else
LIBOOT_LIBRARY   := ../dist/liboot.so
RPATH_FLAG       := -Wl,-rpath,'$$ORIGIN/../dist'
OPENGL_LIBS      := -lGL
LIBRARY_PATH_ENV := LD_LIBRARY_PATH=../dist
endif

all: headless equip_test voicetest asset_test engine_api_test engine_init_test rom_util_test audio_overflow_test enemy_bgm_test fidelity_runner audio_catalog_test audio_sequence_test

headless: headless.c $(LIBOOT_LIBRARY)
	cc -g -rdynamic -I../dist/include headless.c -o headless -L../dist -loot -lm $(RPATH_FLAG)

equip_test: equip_test.c $(LIBOOT_LIBRARY)
	cc -g -rdynamic -I../dist/include equip_test.c -o equip_test -L../dist -loot -lm $(RPATH_FLAG)

# Runtime ROM-audio extraction probe; no copyrighted samples are embedded.
voicetest: voicetest.c $(LIBOOT_LIBRARY)
	cc -g -rdynamic -I../dist/include voicetest.c -o voicetest -L../dist -loot -lm $(RPATH_FLAG)

asset_test: asset_test.c $(LIBOOT_LIBRARY)
	cc -g -rdynamic -I../dist/include asset_test.c -o asset_test -L../dist -loot -lm $(RPATH_FLAG)

engine_api_test: engine_api_test.c $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -I../dist/include engine_api_test.c \
		-o engine_api_test -L../dist -loot -lm $(RPATH_FLAG)

engine_init_test: engine_init_test.c $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -I../dist/include \
		engine_init_test.c -o engine_init_test -L../dist -loot \
		$(RPATH_FLAG)

csharp_init_test: csharp_init_test.cs ../bindings/csharp/LibOot.cs $(LIBOOT_LIBRARY)
	mcs -unsafe -warnaserror -out:csharp_init_test.exe \
		../bindings/csharp/LibOot.cs csharp_init_test.cs
	$(LIBRARY_PATH_ENV) mono csharp_init_test.exe

rom_util_test: rom_util_test.c ../src/rom_util.c ../src/rom_util.h
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -I../src \
		rom_util_test.c ../src/rom_util.c -o rom_util_test

audio_overflow_test: audio_overflow_test.c ../src/audio_math.h
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror \
		audio_overflow_test.c -o audio_overflow_test

enemy_bgm_test: enemy_bgm_test.c $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -I../dist/include \
		enemy_bgm_test.c -o enemy_bgm_test -L../dist -loot \
		$(RPATH_FLAG)

# ROM-backed audio tests take a ROM path at runtime; no copyrighted data is
# embedded in either binary.
audio_catalog_test: audio_catalog_test.c ../src/audio_extract.c ../src/rom_util.c
	# Most legacy LinkVoiceMap rows intentionally zero their trailing timing arrays.
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror \
		-Wno-missing-field-initializers -I../src \
		audio_catalog_test.c ../src/audio_extract.c ../src/rom_util.c -o audio_catalog_test -lm

audio_sequence_test: audio_sequence_test.c $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -I../dist/include \
		audio_sequence_test.c -o audio_sequence_test -L../dist -loot -lm \
		$(RPATH_FLAG)

# Versioned numeric/hash trace recorder and comparator. Its self-test is
# copyright-free; record/compare modes require a user-supplied ROM.
fidelity_runner: fidelity_runner.c $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -Werror -I../dist/include \
		fidelity_runner.c -o fidelity_runner -L../dist -loot -lm \
		$(RPATH_FLAG)
clean:
	rm -f headless equip_test voicetest asset_test engine_api_test engine_init_test rom_util_test audio_overflow_test fidelity_runner audio_catalog_test audio_sequence_test playground csharp_init_test.exe
	rm -rf playground.dSYM
.PHONY: all clean playground

playground: playground.c playground_ui.c playground_ui.h $(LIBOOT_LIBRARY)
	cc -std=c11 -g -Wall -Wextra -Wpedantic -rdynamic -I../dist/include playground.c playground_ui.c -o playground -L../dist -loot $(shell pkg-config --cflags --libs sdl2) $(OPENGL_LIBS) -lm $(RPATH_FLAG)
