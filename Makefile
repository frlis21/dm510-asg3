# SANITY_FLAGS := -fsanitize=address -fsanitize=leak -fsanitize=undefined
CFLAGS := $(SANITY_FLAGS) -O2 -Wall -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=29

.PHONY: clean

tfs: LDFLAGS += -lfuse

all: tfs mktfs
tfs: fuse_tfs.o tfs.o
mktfs: mktfs.o tfs.o

clean:
	rm -f *.o tfs mktfs *.tfs
