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
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>

#define FRAME_SIZE 8192

//
// settings
//
typedef struct settings_t {
    char *device;     // serial device
    int bauds;        // serial baudrate
    char *server;     // remote server address
    int port;         // remote server port
    char *password;   // http request password
    char *logfile;    // raw local log filename
    char *pusher;     // push pipe file

} settings_t;

//
// bundle stuff
//
typedef struct bundle_t {
    int count;
    size_t maxsize;
    char *writer;
    char *buffer;

} bundle_t;

// reset bundle pointer and size
static void bundle_reset(bundle_t *bundle) {
    bundle->count = 0;
    bundle->writer = bundle->buffer;
}

// return current bundle content-size in bytes
static size_t bundle_length(bundle_t *bundle) {
    return bundle->writer - bundle->buffer;
}

// initialize empty bundle
static void bundle_init(bundle_t *bundle) {
    bundle->maxsize = 8192;
    bundle->buffer = (char *) malloc(sizeof(char) * bundle->maxsize);
    bundle_reset(bundle);
}

// append a line to the bundle
static int bundle_append(bundle_t *bundle, char *line) {
    size_t curlen = bundle_length(bundle);
    size_t linelen = strlen(line);

    if(curlen + linelen + 1 > bundle->maxsize) {
        printf("[-] bundle overflow, skipping\n");
        return -1;
    }

    // append the line to the buffer
    sprintf(bundle->writer, "%s\n", line);

    // updating pointer
    bundle->writer += linelen + 1;
    bundle->count += 1;

    return bundle->count;
}

//
// error handling
//
static void diep(char *str) {
    fprintf(stderr, "[-] %s: [%d] %s\n", str, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static void dier(char *str) {
    fprintf(stderr, "[-] %s\n", str);
    exit(EXIT_FAILURE);
}

static int errp(char *str) {
    fprintf(stderr, "[-] %s: [%d] %s\n", str, errno, strerror(errno));
    return -1;
}

//
// device io
//
static char *readfd(int fd, char *buffer, size_t length) {
    int res, saved = 0;
    fd_set readfs;
    int selval;
    char *temp;

    FD_ZERO(&readfs);

    while(1) {
        FD_SET(fd, &readfs);

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
        }

        return buffer;
    }
}

//
// network io
//
static int net_connect(char *host, int port) {
    int sockfd;
    struct sockaddr_in addr_remote;
    struct hostent *hent;

    /* create client socket */
    addr_remote.sin_family = AF_INET;
    addr_remote.sin_port = htons(port);

    /* dns resolution */
    if((hent = gethostbyname(host)) == NULL)
        return errp("gethostbyname");

    memcpy(&addr_remote.sin_addr, hent->h_addr_list[0], hent->h_length);

    if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return errp("socket");

    /* connecting */
    if(connect(sockfd, (const struct sockaddr *) &addr_remote, sizeof(addr_remote)) < 0)
        return errp("connect");

    return sockfd;
}

//
// network request
//
static char *post(settings_t *settings, char *endpoint, bundle_t *bundle) {
    char *frame;
    int sockfd;
    int length;

    printf("[+] posting data\n");

    char *header = "POST %s HTTP/1.0\r\n"
                   "Content-Length: %lu\r\n"
                   "X-GPS-Auth: %s\r\n"
                   "Host: %s\r\n"
                   "\r\n%s";

    if((sockfd = net_connect(settings->server, settings->port)) < 0)
        return NULL;

    if(!(frame = malloc(sizeof(char) * (bundle_length(bundle) + 2048)))) {
        perror("[-] malloc");
        return NULL;
    }

    sprintf(
        frame, header,
        endpoint,
        bundle_length(bundle),
        settings->password,
        settings->server,
        bundle->buffer
    );

    if(send(sockfd, frame, strlen(frame), 0) < 0) {
        perror("[-] send");
        free(frame);
        return NULL;
    }

    free(frame);
    if(!(frame = malloc(sizeof(char) * FRAME_SIZE))) {
        perror("[-] malloc");
        return NULL;
    }

    if((length = recv(sockfd, frame, FRAME_SIZE, 0)) < 0)
        perror("[-] read");

    frame[length] = '\0';
    close(sockfd);

    printf("[+] response: %s\n", frame);

    return frame;
}

// send a post request and wait for a HTTP 200 response
static void validate(settings_t *settings, char *endpoint) {
    char *response;

    // initialize empty bundle
    bundle_t bundle;
    bundle_init(&bundle);

    // sending request and waiting for valid response
    while(!(response = post(settings, endpoint, &bundle))) {
        printf("[-] %s: not reachable, retrying...\n", endpoint);
        usleep(1000000);
    }

    if(strncmp("HTTP/1.1 200 OK", response, 15))
        dier("wrong response from server");

    free(response);
}

//
// main worker
//
static int gpspush(settings_t *settings) {
    int fd;
    char buffer[2048];
    char *response = NULL;
    bundle_t bundle;

    // empty bundle buffer
    bundle_init(&bundle);

    // opening gps-gateway
    if((fd = open(settings->pusher, O_RDONLY)) < 0)
        diep(settings->pusher);

    // connecting to the network
    printf("[+] validating remote server\n");
    validate(settings, "/api/ping");

    // starting a new session
    printf("[+] requesting server new-session\n");
    validate(settings, "/api/push/session");

    while(1) {
        printf("[+] waiting for serial data\n");
        readfd(fd, buffer, sizeof(buffer));

        printf("[+] >> %s\n", buffer);

        // skip invalid header
        if(buffer[0] != '$')
            continue;

        // bundle the line
        if(bundle_append(&bundle, buffer) < 0) {
            // should not happen
            bundle_reset(&bundle);
            continue;
        }

        // sending when RMC frame is received
        if(!strncmp(buffer, "$GPRMC", 6)) {
            // sending bundle over the network
            if(!(response = post(settings, "/api/push/datapoint", &bundle)))
                printf("[-] cannot send datapoint\n");

            free(response);
            bundle_reset(&bundle);
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {
    settings_t settings = {
        .device = "/dev/ttyAMA0",
        .bauds = B9600,
        .server = NULL,
        .port = 80,
        .password = "",
        .logfile = NULL,
        .pusher = "/tmp/gps.pipe",
    };

    if(argc < 2) {
        fprintf(stderr, "[-] missing password\n");
        return 1;
    }

    settings.server = "gps.maxux.net";
    settings.password = argv[1];

    return gpspush(&settings);
}
