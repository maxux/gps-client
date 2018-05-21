CFLAGS  = -W -Wall -O2 -pipe -ansi -std=gnu99
LDFLAGS =

all: gps-gateway gps-push

gps-gateway: gps-gateway.o
	$(CC) -o $@ $^ $(LDFLAGS)

gps-push: gps-push.o

%.o: %.c
	$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -fv *.o

mrproper: clean
	rm -fv gps-gateway gps-push

