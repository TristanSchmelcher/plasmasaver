CC=gcc
CFLAGS=-O2 -Wall -Werror --std=gnu99

plasmasaver: plasmasaver.c
	$(CC) $(CFLAGS) -o $@ $^ $$(pkg-config --cflags --libs gtk+-3.0)

clean:
	rm -f plasmasaver
