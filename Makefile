.PHONY: all install uninstall

CC := gcc
CFLAGS := -std=c99 -pedantic -O3 -D_DEFAULT_SOURCE
INSTALL_DIR := /usr/bin

all: cmfc

install: cmfc
	cp cmfc $(INSTALL_DIR)

uninstall:
	rm $(INSTALL_DIR)/cmfc

cmfc: cmfc.c
	$(CC) $(CFLAGS) -o $@ $<
