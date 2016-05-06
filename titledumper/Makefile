CC=gcc
CFLAGS=-c -Wall -D_FILE_OFFSET_BITS=64
LDFLAGS=
SOURCES:= $(wildcard source/*.c) 
OBJ_FILES = $(patsubst source/%.c,obj/%.o,$(SOURCES))
EXECUTABLE=titledumper

$(EXECUTABLE): $(OBJ_FILES)
	$(CC) $(LDFLAGS) -o $@ $^

obj/%.o: source/%.c
	mkdir obj; $(CC) $(CFLAGS) -c -o $@ $<
