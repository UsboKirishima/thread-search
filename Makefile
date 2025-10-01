TARGET = tsearch
SRC = tsearch.c
CFLAGS = -Wall -pthread

all: $(TARGET)

$(TARGET): $(SRC)
	gcc $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)
