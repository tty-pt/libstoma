FOLDER := stoma
all := libstoma stoma_test stoma_prop_test

LDLIBS-libstoma := -lqmap
libstoma-obj-y := src/token.o
LDLIBS-stoma_test := -lstoma -lqmap
LDLIBS-stoma_prop_test := -lstoma -lqmap

include ../mk/include.mk

test: all
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_test
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_prop_test
