CC = gcc
CFLAGS = -Wextra -Werror -Wall -Wno-gnu-folding-constant -g
SRC = libcoro.c corobus.c test.c utils/unit.c
INCLUDES = -I utils
OUT = test

all:
	$(CC) $(CFLAGS) $(SRC) $(INCLUDES) -o $(OUT)

clean:
	rm -f $(OUT)
