BIN_NAME      := tty-copy

prefix        := $(or $(prefix),$(PREFIX),/usr/local)
bindir        := $(prefix)/bin
mandir        := $(prefix)/share/man

BUILD_DIR     := build

ASCIIDOCTOR   := asciidoctor
INSTALL       := install
LN_S          := ln -s
GIT           := git
SED           := sed

GIT_REV       := $(shell test -d .git && git describe --tags --match 'v*' 2>/dev/null)
ifneq ($(GIT_REV),)
  VERSION     := $(patsubst v%,%,$(GIT_REV))
endif

ifeq ($(DEBUG), 1)
  CFLAGS      := -g -DDEBUG
  CFLAGS      += -Wall -Wextra -pedantic
  ifeq ($(shell $(CC) --version | grep -q clang && echo clang), clang)
    CFLAGS    += -Weverything -Wno-vla
  endif
else
  CFLAGS      ?= -Os -DNDEBUG
endif

D              = $(BUILD_DIR)
MAKEFILE_PATH  = $(lastword $(MAKEFILE_LIST))


all: build

#: Print list of targets.
help:
	@printf '%s\n\n' 'List of targets:'
	@$(SED) -En '/^#:.*/{ N; s/^#: (.*)\n([A-Za-z0-9_-]+).*/\2 \1/p }' $(MAKEFILE_PATH) \
		| while read label desc; do printf '%-15s %s\n' "$$label" "$$desc"; done

.PHONY: help

#: Build sources (the default target).
build: build-exec build-man

#: Build executable.
build-exec: $(D)/$(BIN_NAME)

#: Build man page.
build-man: $(D)/$(BIN_NAME).1

#: Remove generated files.
clean:
	rm -rf "$(D)"

.PHONY: build build-exec build-man clean

#: Install into $DESTDIR.
install: install-exec install-man

#: Install executable into $DESTDIR/$bindir/.
install-exec: build-exec
	$(INSTALL) -D -m755 $(D)/$(BIN_NAME) "$(DESTDIR)$(bindir)/$(BIN_NAME)"

#: Install man page into $DESTDIR/$mandir/man1/.
install-man: build-man
	$(INSTALL) -D -m755 $(D)/$(BIN_NAME).1 "$(DESTDIR)$(mandir)/man1/$(BIN_NAME).1"

#: Uninstall from $DESTDIR.
uninstall:
	rm -f "$(DESTDIR)$(bindir)/$(BIN_NAME)"
	rm -f "$(DESTDIR)$(mandir)/man1/$(BIN_NAME).1"

.PHONY: install install-exec install-man uninstall

#: Update version in zzz.c and README.adoc to $VERSION.
bump-version:
	test -n "$(VERSION)"  # $$VERSION
	$(SED) -E -i "s/^(:version:).*/\1 $(VERSION)/" README.adoc
	$(SED) -E -i "s/(#define\s+VERSION\s+).*/\1\"$(VERSION)\"/" $(BIN_NAME).c

#: Bump version to $VERSION, create release commit and tag.
release: .check-git-clean | bump-version
	test -n "$(VERSION)"  # $$VERSION
	$(GIT) add .
	$(GIT) commit -m "Release version $(VERSION)"
	$(GIT) tag -s v$(VERSION) -m v$(VERSION)

.PHONY: build-version release

$(D)/%.o: %.c | .builddir
	$(CC) $(CFLAGS) -std=c11 $(if $(VERSION),-DVERSION='"$(VERSION)"') -o $@ -c $<

$(D)/%: $(D)/%.o
	$(CC) $(LDFLAGS) -o $@ $<

$(D)/%.1: %.1.adoc | .builddir
	$(ASCIIDOCTOR) -b manpage -o $@ $<

.builddir:
	@mkdir -p "$(D)"

.check-git-clean:
	@test -z "$(shell $(GIT) status --porcelain)" \
		|| { echo 'You have uncommitted changes!' >&2; exit 1; }

.PHONY: .builddir .check-git-clean
