CC      := gcc
PKGCONF := pkg-config
TARGET  := build/statuspop
SRCDIR  := src
BUILDDIR:= build

CFLAGS  := -Wall -Wextra -O2 -std=c11 \
           $(shell $(PKGCONF) --cflags gtk4)
LDFLAGS := $(shell $(PKGCONF) --libs gtk4)

SRCS    := $(wildcard $(SRCDIR)/*.c)
OBJS    := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))

.PHONY: all clean install

all: $(TARGET)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $^ $(LDFLAGS) -o $@
	@echo "Built: $@"

clean:
	rm -rf $(BUILDDIR)

install: all
	install -Dm755 $(TARGET) $(HOME)/.local/bin/statuspop
	@echo "Installed to ~/.local/bin/statuspop"
