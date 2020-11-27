# Makefile for userMode memory management program
#
# Jason Wang
# Summer/Fall 2020

CURRPROG = usermodeMemoryManager
PROGS = $(CURRPROG).exe
OBJS = *.obj
DATASTRUCTURES = ./dataStructures/PTEpermissions.c ./dataStructures/VApermissions.c ./dataStructures/VADNodes.c
COREFUNCTIONS = ./coreFunctions/pageFault.c ./coreFunctions/pageFile.c ./coreFunctions/getPage.c ./coreFunctions/pageTrade.c 
INFRASTRUCTURE = ./infrastructure/bitOps.c ./infrastructure/enqueue-dequeue.c ./infrastructure/jLock.c

SOURCES = $(CURRPROG).c $(DATASTRUCTURES) $(COREFUNCTIONS) $(INFRASTRUCTURE)


CFLAGS = /DEBUG:FULL /Zi
WFLAGS = /W4 /wd4214 /wd4127 /wd4090 /wd4204 /wd4057
CC = cl
MAKE = make
DEL = del /Q		## for windows makefile
# DEL = rm -f 		## for bash makefile


# .PHONY: all clean

all:
	$(CC) $(CFLAGS) $(SOURCES)

no-debug:
	$(CC) $(SOURCES)

warning:
	$(CC) $(CFLAGS) $(WFLAGS) $(SOURCES)


clean:
	$(DEL) $(PROGS)
	$(DEL) *.obj
	$(DEL) *.pdb
	$(DEL) *.ilk


