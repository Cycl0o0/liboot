default: lib

CC      := cc
OOT_DEFINES := -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 \
               -DOOT_VERSION=NTSC_1_2 -DOOT_REVISION=2 \
               -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 \
               -DF3DEX_GBI_2
CFLAGS  := -g -Wall -Wno-unused-function -Wno-unused-variable \
           -fno-strict-aliasing -funsigned-char -fPIC -fvisibility=hidden \
           -DOOT_LIB_EXPORT $(OOT_DEFINES) \
           -Isrc -Isrc/shim -Isrc/decomp/include

HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
SHARED_LIB_EXT    := dylib
SHARED_LINK_FLAGS := -dynamiclib -Wl,-install_name,@rpath/liboot.dylib
else
SHARED_LIB_EXT    := so
SHARED_LINK_FLAGS := -shared
endif
LDFLAGS := -lm $(SHARED_LINK_FLAGS)

SANITIZER_BUILD_DIR ?= build/sanitizers
SANITIZER_C_FLAGS ?=
SANITIZER_LINK_FLAGS ?=

SRC_DIRS  := src src/shim src/gen src/decomp $(shell find src/decomp -type d 2>/dev/null | grep -v include)
BUILD_DIR := build
DIST_DIR  := dist

# decomp TUs use the game's own libc headers (ASSERT etc.); our code uses host libc
$(BUILD_DIR)/src/decomp/%.o: CFLAGS += -Isrc/decomp/include/libc -Isrc/decomp

LIB_FILE          := $(DIST_DIR)/liboot.$(SHARED_LIB_EXT)
LIB_H_FILE        := $(DIST_DIR)/include/liboot.h
LIB_ENGINE_H_FILE := $(DIST_DIR)/include/liboot_engine.h
LIB_CPP_H_FILE    := $(DIST_DIR)/include/liboot.hpp
LIB_LICENSE_FILE  := $(DIST_DIR)/LICENSE
LIB_NOTICE_FILE   := $(DIST_DIR)/NOTICE.md
LIB_README_FILE   := $(DIST_DIR)/README.md
LIB_DOCS_STAMP    := $(DIST_DIR)/.docs-stamp

DOC_SUPPORT_FILES := docs/GETTING_STARTED.md docs/UNIVERSAL_SDK.md \
                     docs/ENGINE_INTEGRATION.md docs/FIDELITY.md bindings/README.md \
                     bindings/cpp/liboot.hpp bindings/csharp/LibOot.cs \
                     bindings/csharp/README.md \
                     examples/engine.c examples/basic.c src/liboot.h \
                     src/liboot_engine.h

C_FILES := $(sort $(filter-out %.inc.c,$(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))))
O_FILES := $(foreach f,$(C_FILES),$(BUILD_DIR)/$(f:.c=.o))
DEP_FILES := $(O_FILES:.o=.d)

DUMMY := $(shell mkdir -p $(addprefix $(BUILD_DIR)/,$(SRC_DIRS)) $(DIST_DIR)/include)

lib: $(LIB_FILE) $(LIB_H_FILE) $(LIB_ENGINE_H_FILE) $(LIB_CPP_H_FILE) \
     $(LIB_LICENSE_FILE) $(LIB_NOTICE_FILE) $(LIB_README_FILE) $(LIB_DOCS_STAMP)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -c $< -o $@

$(LIB_FILE): $(O_FILES)
	$(CC) $^ $(LDFLAGS) -o $@

$(LIB_H_FILE): src/liboot.h
	cp $< $@

$(LIB_ENGINE_H_FILE): src/liboot_engine.h
	cp $< $@

$(LIB_CPP_H_FILE): bindings/cpp/liboot.hpp
	cp $< $@

$(LIB_LICENSE_FILE): LICENSE
	cp $< $@

$(LIB_NOTICE_FILE): NOTICE.md
	cp $< $@

$(LIB_README_FILE): README.md
	cp $< $@

$(LIB_DOCS_STAMP): $(DOC_SUPPORT_FILES)
	mkdir -p $(DIST_DIR)/docs $(DIST_DIR)/src $(DIST_DIR)/bindings/cpp \
	           $(DIST_DIR)/bindings/csharp $(DIST_DIR)/examples
	cp docs/GETTING_STARTED.md docs/UNIVERSAL_SDK.md \
	   docs/ENGINE_INTEGRATION.md docs/FIDELITY.md $(DIST_DIR)/docs/
	cp src/liboot.h src/liboot_engine.h $(DIST_DIR)/src/
	cp bindings/README.md $(DIST_DIR)/bindings/
	cp bindings/cpp/liboot.hpp $(DIST_DIR)/bindings/cpp/
	cp bindings/csharp/LibOot.cs bindings/csharp/README.md \
	   $(DIST_DIR)/bindings/csharp/
	cp examples/engine.c examples/basic.c $(DIST_DIR)/examples/
	touch $@

example: lib
	$(MAKE) -C examples

test: lib
	$(MAKE) -C test

# Keep sanitizer objects isolated from the native Make build. Optional
# SANITIZER_C_FLAGS/SANITIZER_LINK_FLAGS let a non-system compiler runtime be
# supplied without baking a machine-specific path into the project.
sanitizers:
	cmake -S . -B $(SANITIZER_BUILD_DIR) \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=ON \
		-DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DLIBOOT_ENABLE_SANITIZERS=ON \
		-DCMAKE_C_FLAGS="$(SANITIZER_C_FLAGS)" \
		-DCMAKE_EXE_LINKER_FLAGS="$(SANITIZER_LINK_FLAGS)"
	cmake --build $(SANITIZER_BUILD_DIR) --parallel
	ASAN_OPTIONS="$${ASAN_OPTIONS:-detect_leaks=1:halt_on_error=1}" \
	UBSAN_OPTIONS="$${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
		ctest --test-dir $(SANITIZER_BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)

-include $(DEP_FILES)

.PHONY: default lib test sanitizers example clean
