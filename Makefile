CC ?= cc
HOSTCC ?= cc
AR ?= ar
PREFIX ?= /usr/local
DESTDIR ?=
EMBED_LOADERS ?= 0
TOOLCHAIN_TAG ?= $(notdir $(firstword $(CC)))
BUILD_DIR ?= build/$(TOOLCHAIN_TAG)

CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
LDLIBS ?=
HOSTCFLAGS ?= -O2

CPPFLAGS += -Iinclude -Ilib/argparse -Ilib/progressbar/include/progressbar -D_GNU_SOURCE
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fPIC

ifeq ($(SANITIZE),1)
CFLAGS += -O1 -fno-omit-frame-pointer -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

ifneq ($(filter $(EMBED_LOADERS),0 1),$(EMBED_LOADERS))
$(error EMBED_LOADERS must be 0 or 1)
endif

CORE_SRC := src/util.c src/serial.c src/loader.c src/protocol.c src/commands.c src/library.c
CORE_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(CORE_SRC))
PROGRESS_OBJ := $(BUILD_DIR)/lib/progressbar/lib/progressbar.o
APP_SRC := src/main.c lib/argparse/argparse.c
APP_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(APP_SRC))
LIB_STATIC := libgxdl.a
LIB_SHARED := libgxdl.so
EXAMPLE := $(BUILD_DIR)/examples/basic
PC_FILE := $(BUILD_DIR)/libgxdl.pc
API_TEST := $(BUILD_DIR)/tests/public_api

LOADERS := $(sort $(wildcard lib/loaders/*.boot))
LOADER_STEMS := $(basename $(notdir $(LOADERS)))
BIN2C := $(BUILD_DIR)/tools/bin2c
GEN_DIR := $(BUILD_DIR)/generated
GEN_STAMP := $(GEN_DIR)/loaders.stamp
GEN_REGISTRY := $(GEN_DIR)/embedded_registry.c
GEN_LOADER_C := $(addprefix $(GEN_DIR)/loader_,$(addsuffix .c,$(LOADER_STEMS)))
GEN_OBJ := $(patsubst %.c,%.o,$(GEN_LOADER_C) $(GEN_REGISTRY))

ifeq ($(EMBED_LOADERS),1)
ifeq ($(strip $(LOADERS)),)
$(error EMBED_LOADERS=1 but no lib/loaders/*.boot files were found)
endif
LIB_OBJ := $(CORE_OBJ) $(PROGRESS_OBJ) $(GEN_OBJ)
else
LIB_OBJ := $(CORE_OBJ) $(PROGRESS_OBJ) $(BUILD_DIR)/src/embedded_none.o
endif

.PHONY: all libs example install uninstall clean test FORCE
all: gxdl-c libs

libs: $(LIB_STATIC) $(LIB_SHARED)

gxdl-c: FORCE $(APP_OBJ) $(LIB_STATIC)
	$(CC) $(LDFLAGS) $(APP_OBJ) $(LIB_STATIC) $(LDLIBS) -o $@

$(LIB_STATIC): FORCE $(LIB_OBJ)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJ)

$(LIB_SHARED): FORCE $(LIB_OBJ)
	$(CC) -shared $(LDFLAGS) $(LIB_OBJ) $(LDLIBS) -o $@

$(BUILD_DIR)/%.o: %.c include/gxdl.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/lib/argparse/argparse.o: lib/argparse/argparse.c lib/argparse/argparse.h
$(BUILD_DIR)/lib/progressbar/lib/progressbar.o: lib/progressbar/lib/progressbar.c lib/progressbar/include/progressbar/progressbar.h
$(BUILD_DIR)/src/library.o: src/library.c include/gxdl/gxdl.h include/gxdl.h
$(BUILD_DIR)/lib/argparse/argparse.o $(BUILD_DIR)/lib/progressbar/lib/progressbar.o: CFLAGS += -Wno-conversion -Wno-sign-conversion

$(BIN2C): lib/bin2c/src/bin2c.c lib/bin2c/src/bin2c.h lib/bin2c/src/help_dummy.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -std=c11 $(HOSTCFLAGS) -Wall -Wextra -DBIN2C_HEADER_ONLY=1 -Ilib/bin2c/src \
		lib/bin2c/src/bin2c.c lib/bin2c/src/help_dummy.c -o $@

FORCE:

$(GEN_STAMP): FORCE scripts/gen_loaders.sh $(BIN2C) $(LOADERS)
	@mkdir -p $(GEN_DIR)
	sh scripts/gen_loaders.sh $(GEN_DIR) $(BIN2C) $(LOADERS)
	@touch $@

$(GEN_LOADER_C) $(GEN_REGISTRY): $(GEN_STAMP)
	@test -f $@

$(GEN_DIR)/%.o: $(GEN_DIR)/%.c include/gxdl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-overlength-strings -c $< -o $@

UNIT_TEST := $(BUILD_DIR)/tests/unit
UNIT_OBJ := $(CORE_OBJ) \
	$(BUILD_DIR)/src/embedded_none.o $(BUILD_DIR)/tests/unit.o \
	$(PROGRESS_OBJ)

$(UNIT_TEST): $(UNIT_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(EXAMPLE): examples/basic.c $(LIB_STATIC) include/gxdl/gxdl.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) examples/basic.c $(LIB_STATIC) $(LDFLAGS) $(LDLIBS) -o $@

example: $(EXAMPLE)

$(API_TEST): tests/public_api.c $(LIB_SHARED) include/gxdl/gxdl.h
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/public_api.c -L. -lgxdl $(LDFLAGS) $(LDLIBS) -o $@

$(PC_FILE): libgxdl.pc.in Makefile
	@mkdir -p $(dir $@)
	sed 's|@PREFIX@|$(PREFIX)|g' $< > $@

test: gxdl-c libs example $(UNIT_TEST) $(API_TEST)
	$(UNIT_TEST)
	LD_LIBRARY_PATH=. $(API_TEST)
	python3 tests/pty_integration.py ./gxdl-c

install: all $(PC_FILE)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib \
		$(DESTDIR)$(PREFIX)/lib/pkgconfig $(DESTDIR)$(PREFIX)/include/gxdl
	install -m 755 gxdl-c $(DESTDIR)$(PREFIX)/bin/gxdl-c
	install -m 644 $(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/$(LIB_STATIC)
	install -m 755 $(LIB_SHARED) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED)
	install -m 644 $(PC_FILE) $(DESTDIR)$(PREFIX)/lib/pkgconfig/libgxdl.pc
	install -m 644 include/gxdl/gxdl.h $(DESTDIR)$(PREFIX)/include/gxdl/gxdl.h

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gxdl-c
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB_STATIC) $(DESTDIR)$(PREFIX)/lib/$(LIB_SHARED)
	rm -f $(DESTDIR)$(PREFIX)/lib/pkgconfig/libgxdl.pc
	rm -f $(DESTDIR)$(PREFIX)/include/gxdl/gxdl.h

clean:
	rm -rf build gxdl-c $(LIB_STATIC) $(LIB_SHARED)
