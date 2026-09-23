FOLDER := stoma
all := libstoma stoma_test stoma_prop_test stoma_axis_store_test stoma_axis_roundtrip_test stoma_axis_rebuild_test

LDLIBS-libstoma := -lcorm
libstoma-obj-y := src/token.o
LDLIBS-stoma_test := -lstoma -lcorm
LDLIBS-stoma_prop_test := -lstoma -lcorm
LDLIBS-stoma_axis_store_test := -lstoma -lcorm
LDLIBS-stoma_axis_roundtrip_test := -lstoma -lcorm
LDLIBS-stoma_axis_rebuild_test := -lstoma -lcorm -lqsys

CFLAGS += -I/home/quirinpa/site/external/libcorm/include

include ../mk/include.mk

test: all
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_test
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_prop_test
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_axis_store_test
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_axis_roundtrip_test
	rm -rf /tmp/stoma-rebuild-test-*
	LD_LIBRARY_PATH=$(abspath lib) ./bin/stoma_axis_rebuild_test
	rm -rf /tmp/stoma-rebuild-test-*
