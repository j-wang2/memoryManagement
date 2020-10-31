# Makefile for userMode memory management program
#
# Jason Wang
# Summer/Fall 2020

CURRPROG = userMode-AWE-pageFile
PROGS = $(CURRPROG).exe
OBJS = *.obj

SOURCES = $(CURRPROG).c enqueue-dequeue.c pageFault.c pageFile.c getPage.c PTEpermissions.c VApermissions.c pageTrade.c jLock.c VADNodes.c bitOps.c


CFLAGS = /DEBUG:FULL /Zi
WFLAGS = /W4 /wd4214 /wd4127 /wd4090
CC = cl
MAKE = make
DEL = del /Q		## for windows makefile
# DEL = rm -f 		## for bash makefile


# .PHONY: all clean

all:
	$(CC) $(CFLAGS)  $(SOURCES)

no-debug:
	$(CC) $(SOURCES)

warning:
	$(CC) $(CFLAGS) $(WFLAGS) $(SOURCES)


clean:
	$(DEL) $(PROGS)
	$(DEL) *.obj
	$(DEL) *.pdb
	$(DEL) *.ilk
