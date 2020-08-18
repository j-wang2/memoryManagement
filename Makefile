# Makefile for userMode memory management program
#
# Jason Wang
# Summer/Fall 2020

CURRPROG = userMode-AWE-pageFile
PROGS = $(CURRPROG).exe
OBJS = *.obj
SOURCES = $(CURRPROG).c enqueue-dequeue.c pageFile.c


CFLAGS = /DEBUG:FULL /Zi
CC = cl
MAKE = make
DEL = del /Q		## for windows makefile
# DEL = rm -f 		## for bash makefile


# .PHONY: all clean

all:
	$(CC) $(CFLAGS) $(SOURCES)

clean:
	$(DEL) $(PROGS)
	$(DEL) *.obj
	$(DEL) *.pdb
	$(DEL) *.ilk
