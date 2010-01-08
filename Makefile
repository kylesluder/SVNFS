CFLAGS += -D_FILE_OFFSET_BITS=64 -g -Wall -Werror -I/usr/include/apr-0 -I/usr/include/subversion-1 -I/usr/include/fuse

.PHONY: all clean

all: svnfs

clean:
	$(RM) svnfs svnfs.o *core*

svnfs: -lfuse
svnfs: -lsvn_client-1
svnfs: svnfs.o
