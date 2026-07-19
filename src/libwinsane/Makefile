CC = gcc
WINDRES = windres

all: libwinsane.o

libwinsane.o: init.o manifest.o
	$(CC) -r -o $@ $^

init.o: init.c
	$(CC) -c -o $@ $<

manifest.o: manifest.rc utf8.xml
	$(WINDRES) -O coff -o $@ manifest.rc

clean:
	$(RM) init.o manifest.o libwinsane.o

.PHONY: all clean
