# Eclipse - a friendly Unix shell
#
#   make                build ./eclipse
#   make test           build and run every test suite
#   make install        install to ~/.local (no root needed)
#   make install-config create ~/.config/eclipse/config if it is missing
#   make uninstall      remove what `make install` installed
#   make debug          build with AddressSanitizer and UBSan
#   make test-debug     run every test suite under the sanitizers
#   make clean
#
# Override PREFIX for a different location, e.g.
#   sudo make install PREFIX=/usr/local

CC      ?= cc
CFLAGS  ?= -O2 -g
PREFIX  ?= $(HOME)/.local
BINDIR   = $(PREFIX)/bin
MANDIR   = $(PREFIX)/share/man/man1
DOCDIR   = $(PREFIX)/share/doc/eclipse

CONFIG_HOME = $(if $(XDG_CONFIG_HOME),$(XDG_CONFIG_HOME),$(HOME)/.config)
CONFIG_FILE = $(CONFIG_HOME)/eclipse/config

BUILD   ?= build

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
           -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
HARDEN  ?= -D_FORTIFY_SOURCE=2 -fstack-protector-strong -fPIE
HARDEN_LD ?= -pie -Wl,-z,relro,-z,now

ALL_CFLAGS  = -std=c17 -D_GNU_SOURCE $(WARNINGS) $(HARDEN) $(CFLAGS) $(CPPFLAGS)
ALL_LDFLAGS = $(HARDEN_LD) $(LDFLAGS)

SRC  = $(wildcard src/*.c)
OBJ  = $(SRC:src/%.c=$(BUILD)/%.o)
DEP  = $(OBJ:.o=.d)
# everything except main(), for linking into the unit tests
LIBOBJ = $(filter-out $(BUILD)/main.o,$(OBJ))

.PHONY: all test check test-unit test-integration test-pty debug test-debug \
        install install-config uninstall clean

all: eclipse

eclipse: $(OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJ) $(ALL_LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

-include $(DEP)

# ---- tests ----------------------------------------------------------

$(BUILD)/test_unit: tests/test_unit.c $(LIBOBJ)
	$(CC) $(ALL_CFLAGS) -Isrc -o $@ tests/test_unit.c $(LIBOBJ) $(ALL_LDFLAGS)

$(BUILD)/test_pty: tests/test_pty.c | $(BUILD)
	$(CC) $(ALL_CFLAGS) -o $@ tests/test_pty.c $(ALL_LDFLAGS)

test-unit: $(BUILD)/test_unit
	$(BUILD)/test_unit

test-integration: eclipse
	sh tests/integration.sh ./eclipse

test-pty: eclipse $(BUILD)/test_pty
	$(BUILD)/test_pty ./eclipse

test: test-unit test-integration test-pty
	@echo "All test suites passed."

check: test

# A separate build tree with AddressSanitizer and UBSan. `make test-debug`
# builds it and runs all three suites against it.
# (-Wno-format-truncation: GCC reports a false "null format string" for
# vsnprintf(NULL, 0, ...) only when the sanitizers are enabled.)
SANITIZE = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
           -Wno-format-truncation
DEBUG_MAKE = $(MAKE) BUILD=build-debug HARDEN= HARDEN_LD= CFLAGS="$(SANITIZE)"

debug:
	$(DEBUG_MAKE) build-debug/eclipse-debug

test-debug:
	$(DEBUG_MAKE) build-debug/eclipse-debug build-debug/test_unit build-debug/test_pty
	UBSAN_OPTIONS=halt_on_error=1 build-debug/test_unit
	UBSAN_OPTIONS=halt_on_error=1 sh tests/integration.sh build-debug/eclipse-debug
	UBSAN_OPTIONS=halt_on_error=1 build-debug/test_pty build-debug/eclipse-debug
	@echo "All test suites passed under the sanitizers."

build-debug/eclipse-debug: $(OBJ)
	$(CC) $(ALL_CFLAGS) -o $@ $(OBJ) $(ALL_LDFLAGS)

# ---- install --------------------------------------------------------

install: eclipse
	install -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)" "$(DESTDIR)$(DOCDIR)"
	install -m 755 eclipse "$(DESTDIR)$(BINDIR)/eclipse"
	install -m 644 docs/eclipse.1 "$(DESTDIR)$(MANDIR)/eclipse.1"
	install -m 644 config/config.example "$(DESTDIR)$(DOCDIR)/config.example"
	@echo
	@echo "Installed $(DESTDIR)$(BINDIR)/eclipse"
	@echo "Your login shell has not been changed. Start Eclipse by running: eclipse"
	@case ":$$PATH:" in *":$(BINDIR):"*) ;; \
	  *) echo "Note: $(BINDIR) is not in your PATH; add it to run 'eclipse' by name." ;; esac

# Never overwrites an existing config.
install-config:
	@if [ -e "$(CONFIG_FILE)" ]; then \
	    echo "$(CONFIG_FILE) already exists; leaving it alone."; \
	else \
	    install -d -m 700 "$(dir $(CONFIG_FILE))" && \
	    install -m 600 config/config.example "$(CONFIG_FILE)" && \
	    echo "Created $(CONFIG_FILE)"; \
	fi

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/eclipse" "$(DESTDIR)$(MANDIR)/eclipse.1" \
	      "$(DESTDIR)$(DOCDIR)/config.example"
	-rmdir "$(DESTDIR)$(DOCDIR)" 2>/dev/null
	@echo "Removed Eclipse. Your config and history were left in place:"
	@echo "  $(CONFIG_FILE)"
	@echo "  $(HOME)/.local/share/eclipse/history"

clean:
	rm -rf build build-debug eclipse
