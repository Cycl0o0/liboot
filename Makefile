default: lib

CC      := cc
PYTHON3 ?= python3
OOT_DEFINES := -DLIBOOT_HOST_BUILD=1 -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB -DDEBUG_FEATURES=0 \
               -DOOT_VERSION=PAL_1_1 -DOOT_REVISION=1 \
               -DPLATFORM_N64=1 -DPLATFORM_GC=0 -DPLATFORM_IQUE=0 \
               -DF3DEX_GBI_2 \
               -DosCreateMesgQueue=liboot_internal_osCreateMesgQueue \
               -DosSendMesg=liboot_internal_osSendMesg \
               -DosRecvMesg=liboot_internal_osRecvMesg \
               -DguPerspective=liboot_internal_guPerspective
CFLAGS  := -g -Wall -Wno-unused-function -Wno-unused-variable \
           -fno-strict-aliasing -funsigned-char -fPIC -fvisibility=hidden \
           -DOOT_LIB_EXPORT -DLIBOOT_MULTI_INSTANCE=1 $(OOT_DEFINES) \
           -Isrc -Isrc/shim -Isrc/decomp/include -Isrc/decomp/src

HOST_OS := $(shell uname -s)
ifeq ($(HOST_OS),Darwin)
SHARED_LIB_EXT    := dylib
SHARED_LINK_FLAGS := -dynamiclib -Wl,-install_name,@rpath/liboot.dylib
ASAN_DEFAULT_OPTIONS := detect_leaks=0:halt_on_error=1
else
SHARED_LIB_EXT    := so
SHARED_LINK_FLAGS := -shared -Wl,-z,now
ASAN_DEFAULT_OPTIONS := detect_leaks=1:halt_on_error=1
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
LIB_MIXER_LICENSE_FILE := $(DIST_DIR)/LICENSES/LibUltraShip-MIT.txt
LIB_NOTICE_FILE   := $(DIST_DIR)/NOTICE.md
LIB_README_FILE   := $(DIST_DIR)/README.md
LIB_CHANGELOG_FILE := $(DIST_DIR)/CHANGELOG.md
LIB_CONTRIBUTING_FILE := $(DIST_DIR)/CONTRIBUTING.md
LIB_CONTRIBUTORS_FILE := $(DIST_DIR)/CONTRIBUTORS.md
LIB_SECURITY_FILE := $(DIST_DIR)/SECURITY.md
LIB_DOCS_STAMP    := $(DIST_DIR)/.docs-stamp

DOC_SUPPORT_FILES := CHANGELOG.md CONTRIBUTING.md CONTRIBUTORS.md SECURITY.md \
                     docs/README.md docs/GETTING_STARTED.md docs/USAGE.md \
                     docs/API_REFERENCE.md docs/UNIVERSAL_SDK.md \
                     docs/ENGINE_INTEGRATION.md docs/FIDELITY.md \
                     docs/ROM_COMPATIBILITY.md docs/DEVELOPMENT.md \
                     docs/RELEASING.md bindings/README.md \
                     bindings/cpp/liboot.hpp bindings/csharp/LibOot.cs \
                     bindings/csharp/README.md \
                     examples/engine.c examples/basic.c src/liboot.h \
                     src/liboot_engine.h tools/check-install.sh \
                     tools/identify-rom.py \
                     tools/rom-profiles.json fuzz/README.md

C_FILES := $(sort $(filter-out %.inc.c,$(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c))))
O_FILES := $(foreach f,$(C_FILES),$(BUILD_DIR)/$(f:.c=.o))
DEP_FILES := $(O_FILES:.o=.d)

DUMMY := $(shell mkdir -p $(addprefix $(BUILD_DIR)/,$(SRC_DIRS)) $(DIST_DIR)/include)

lib: $(LIB_FILE) $(LIB_H_FILE) $(LIB_ENGINE_H_FILE) $(LIB_CPP_H_FILE) \
     $(LIB_LICENSE_FILE) $(LIB_MIXER_LICENSE_FILE) $(LIB_NOTICE_FILE) $(LIB_README_FILE) \
     $(LIB_CHANGELOG_FILE) $(LIB_CONTRIBUTING_FILE) $(LIB_CONTRIBUTORS_FILE) \
     $(LIB_SECURITY_FILE) \
     $(LIB_DOCS_STAMP)

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

$(LIB_MIXER_LICENSE_FILE): LICENSES/LibUltraShip-MIT.txt
	@mkdir -p $(dir $@)
	cp $< $@

$(LIB_NOTICE_FILE): NOTICE.md
	cp $< $@

$(LIB_README_FILE): README.md
	cp $< $@

$(LIB_CHANGELOG_FILE): CHANGELOG.md
	cp $< $@

$(LIB_CONTRIBUTING_FILE): CONTRIBUTING.md
	cp $< $@

$(LIB_CONTRIBUTORS_FILE): CONTRIBUTORS.md
	cp $< $@

$(LIB_SECURITY_FILE): SECURITY.md
	cp $< $@

$(LIB_DOCS_STAMP): $(DOC_SUPPORT_FILES)
	mkdir -p $(DIST_DIR)/docs $(DIST_DIR)/src $(DIST_DIR)/bindings/cpp \
	           $(DIST_DIR)/bindings/csharp $(DIST_DIR)/examples \
	           $(DIST_DIR)/fuzz $(DIST_DIR)/tools
	cp docs/README.md docs/GETTING_STARTED.md docs/USAGE.md \
	   docs/API_REFERENCE.md docs/UNIVERSAL_SDK.md \
	   docs/ENGINE_INTEGRATION.md docs/FIDELITY.md \
	   docs/ROM_COMPATIBILITY.md docs/DEVELOPMENT.md docs/RELEASING.md \
	   $(DIST_DIR)/docs/
	cp src/liboot.h src/liboot_engine.h $(DIST_DIR)/src/
	cp bindings/README.md $(DIST_DIR)/bindings/
	cp bindings/cpp/liboot.hpp $(DIST_DIR)/bindings/cpp/
	cp bindings/csharp/LibOot.cs bindings/csharp/README.md \
	   $(DIST_DIR)/bindings/csharp/
	cp examples/engine.c examples/basic.c $(DIST_DIR)/examples/
	cp fuzz/README.md $(DIST_DIR)/fuzz/
	cp tools/check-install.sh tools/identify-rom.py tools/rom-profiles.json \
	   $(DIST_DIR)/tools/
	touch $@

example: lib
	$(MAKE) -C examples

# Compile every test program; ROM-backed programs are not run here.
test: lib
	$(MAKE) -C test

check: lib
	tools/check-version.sh
	$(PYTHON3) tools/check-bindings.py
	$(PYTHON3) tools/check-vendor.py
	$(MAKE) -C test check PYTHON3="$(PYTHON3)"

fuzz-smoke:
	$(MAKE) -C fuzz smoke

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
	ASAN_OPTIONS="$${ASAN_OPTIONS:-$(ASAN_DEFAULT_OPTIONS)}" \
	UBSAN_OPTIONS="$${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}" \
		ctest --test-dir $(SANITIZER_BUILD_DIR) --output-on-failure

clean:
	$(MAKE) -C examples clean
	$(MAKE) -C test clean
	rm -rf $(BUILD_DIR) $(DIST_DIR)

-include $(DEP_FILES)

.PHONY: default lib test check fuzz-smoke sanitizers example clean
