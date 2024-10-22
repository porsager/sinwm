TARGET = sinwm
SRC = sinwm.c

all:
	gcc -o $(TARGET) $(SRC) -lxcb -lxcb-icccm -lxcb-randr -lxcb-image -lpng

install:
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

clean:
	rm -f $(TARGET)
