/*
 * Author: Daniel Maxime (root@maxux.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>

//
// error handling
//
void diep(char *str) {
	fprintf(stderr, "[-] %s: [%d] %s\n", str, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

void dier(char *str) {
	fprintf(stderr, "[-] %s\n", str);
	exit(EXIT_FAILURE);
}

//
// device setter
//
int set_interface_attribs(int fd, int speed) {
	struct termios tty;

	memset(&tty, 0, sizeof(tty));

	if(tcgetattr(fd, &tty) != 0)
		diep("tcgetattr");

	tty.c_cflag = speed | CRTSCTS | CS8 | CLOCAL | CREAD;

	tty.c_iflag  = IGNPAR | ICRNL;
	tty.c_lflag  = ICANON;
	tty.c_oflag  = 0;
	tty.c_cc[VMIN]  = 1;
	tty.c_cc[VTIME] = 0;

	if(tcsetattr(fd, TCSANOW, &tty) != 0)
		diep("tcgetattr");

	return 0;
}

//
// device i/o
//
char *readfd(int fd, char *buffer, size_t length) {
	int res, saved = 0;
	fd_set readfs;
	int selval;
	struct timeval tv, *ptv;
	char *temp;

	FD_ZERO(&readfs);

	while(1) {
		FD_SET(fd, &readfs);

		tv.tv_sec  = 2;
		tv.tv_usec = 0;

        ptv = NULL; // ptv = &tv;

		if((selval = select(fd + 1, &readfs, NULL, NULL, ptv)) < 0)
			diep("select");

		if(FD_ISSET(fd, &readfs)) {
			if((res = read(fd, buffer + saved, length - saved)) < 0)
				diep("fd read");

			buffer[res + saved] = '\0';

			// line/block is maybe not completed, waiting for a full line/block
			if(buffer[res + saved - 1] != '\n') {
				saved = res;
				continue;
			}

			buffer[res + saved - 1] = '\0';

			for(temp = buffer; *temp == '\n'; temp++);
			memmove(buffer, temp, strlen(temp));

			if(!*buffer)
				continue;

			printf("[+] >> %s\n", buffer);

		}

		return buffer;
	}
}

int main(int argc, char *argv[]) {
    int fd;
    char *device = "/dev/ttyAMA0";
    char buffer[2048];

    if((fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
		diep(device);

	set_interface_attribs(fd, B9600);

    while(1) {
        readfd(fd, buffer, sizeof(buffer));
    }

    return 0;
}
