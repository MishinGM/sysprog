CC = gcc
CFLAGS = -Wall -Wextra -Werror -Wno-unused-function
INCLUDES = -I ../utils
SRCS = userfs.c test.c ../utils/unit.c
TARGET = test

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) $(INCLUDES) -o $(TARGET)

clean:
	rm -f $(TARGET)
