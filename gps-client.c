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
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
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

int errp(char *str) {
    perror(str);
    return -1;
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
// device io
//
char *readfd(int fd, char *buffer, size_t length) {
    int res, saved = 0;
    fd_set readfs;
    int selval;
    // struct timeval tv, *ptv;
    char *temp;

    FD_ZERO(&readfs);

    while(1) {
        FD_SET(fd, &readfs);

        // tv.tv_sec  = 2;
        // tv.tv_usec = 0;

        // ptv = NULL; // ptv = &tv;

        if((selval = select(fd + 1, &readfs, NULL, NULL, NULL)) < 0)
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
            fflush(stdout);
        }

        return buffer;
    }
}

//
// network io
//
int net_connect(char *host, int port) {
	int sockfd;
	struct sockaddr_in addr_remote;
	struct hostent *hent;

	/* create client socket */
	addr_remote.sin_family = AF_INET;
	addr_remote.sin_port = htons(port);

	/* dns resolution */
	if((hent = gethostbyname(host)) == NULL)
		return errp("[-] gethostbyname");

	memcpy(&addr_remote.sin_addr, hent->h_addr_list[0], hent->h_length);

	if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
		return errp("[-] socket");

	/* connecting */
	if(connect(sockfd, (const struct sockaddr *) &addr_remote, sizeof(addr_remote)) < 0)
		return errp("[-] connect");

	return sockfd;
}

//
// network request
//
char *http(char *endpoint) {
    char *server = "home.maxux.net";
    int port = 5555;

    char header[2048];
    char buffer[256];
    int sockfd;
    int length;

    sprintf(header, "GET %s\r\n\r\n", endpoint);

    if((sockfd = net_connect(server, port)) < 0)
        return NULL;

    if(send(sockfd, header, strlen(header), 0) < 0) {
        perror("[-] send");
        return NULL;
    }

    if((length = recv(sockfd, buffer, sizeof(buffer), 0)) < 0)
        perror("[-] read");

    buffer[length] = '\0';
    printf("[+] buffer: %s\n", buffer);

    return strdup(buffer);
}

//
// local logs
//
int logs_create() {
    int fd;
    char filename[256];

    sprintf(filename, "/mnt/backlog/gps-%lu", time(NULL));
    if((fd = open(filename, O_WRONLY | O_CREAT, 0644)) < 0)
        return errp(filename);

    return fd;
}

void logs_append(int fd, char *line) {
    if(write(fd, line, strlen(line)) < 0)
        perror("[-] logs write");
}

//
// main worker
//
int main(void) {
    int fd, logsfd;
    char *device = "/dev/ttyAMA0";
    char buffer[2048];
    char *response = NULL;
    char request[1024];

    if((logsfd = logs_create() < 0))
        diep("[-] logs");

    // connecting to the network
    while(!(response = http("/api/ping"))) {
        printf("[-] could not reach endpoint, waiting\n");
        usleep(1000000);
    }

    if(strncmp("{\"pong\"", response, 7))
        dier("wrong pong response from server");

    free(response);

    // setting up serial console
    if((fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
        diep(device);

    set_interface_attribs(fd, B9600);

    // starting a new session
    if(!(response = http("/api/push/session")))
        dier("cannot create new session");

    while(1) {
        readfd(fd, buffer, sizeof(buffer));

        // skip invalid header
        if(buffer[0] != '$')
            continue;

        // saving to local logs
        logs_append(logsfd, buffer);

        // sending over the network
        sprintf(request, "/api/push/data/%s", buffer);
        if(!(response = http(request)))
            printf("[-] cannot send datapoint\n");
    }

    return 0;
}
