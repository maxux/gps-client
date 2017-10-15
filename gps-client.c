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
// device setter
//
static int serialfd(char *device, int bauds) {
    struct termios tty;
    int fd;

    if((fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0)
        diep(device);

    memset(&tty, 0, sizeof(tty));

    if(tcgetattr(fd, &tty) != 0)
        diep("tcgetattr");

    tty.c_cflag = bauds | CRTSCTS | CS8 | CLOCAL | CREAD;
    tty.c_iflag = IGNPAR | ICRNL;
    tty.c_lflag = ICANON;
    tty.c_oflag = 0;
    tty.c_cc[VMIN]  = 1;
    tty.c_cc[VTIME] = 0;

    if(tcsetattr(fd, TCSANOW, &tty) != 0)
        diep("tcgetattr");

    return fd;
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
    char frame[8192];
    int sockfd;
    int length;

    printf("[+] posting data\n");

    char *header = "POST %s HTTP/1.0\r\n"
                   "Content-Length: %lu\r\n"
                   "X-GPS-Auth: %s\r\n"
                   "Host: %s\r\n"
                   "\r\n%s";

    sprintf(
        frame, header,
        endpoint,
        bundle_length(bundle),
        settings->password,
        settings->server,
        bundle->buffer
    );

    if((sockfd = net_connect(settings->server, settings->port)) < 0)
        return NULL;

    if(send(sockfd, frame, strlen(frame), 0) < 0) {
        perror("[-] send");
        return NULL;
    }

    if((length = recv(sockfd, frame, sizeof(frame), 0)) < 0)
        perror("[-] read");

    frame[length] = '\0';
    close(sockfd);

    printf("[+] response: %s\n", frame);

    return strdup(frame);
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
// local logs
//
static int logs_index_get(char *indexfile) {
    FILE *fp;
    char buffer[32];

    if(!(fp = fopen(indexfile, "r")))
        return 0;

    if(!fread(buffer, sizeof(buffer), 1, fp))
        diep(indexfile);

    fclose(fp);

    return atoi(buffer);
}

static int logs_index_set(char *indexfile, int value) {
    FILE *fp;
    char buffer[32];

    sprintf(buffer, "%d", value);

    if(!(fp = fopen(indexfile, "w")))
        return 0;

    if(!fwrite(buffer, sizeof(buffer), 1, fp))
        diep(indexfile);

    fclose(fp);

    return value;
}

static int logs_index(char *storage) {
    char filename[256];
    int index;

    // set index filename
    sprintf(filename, "%s/index", storage);

    // loads index and increment it
    index = logs_index_get(filename);
    logs_index_set(filename, index + 1);

    return index;
}

static int logs_create(char *filename) {
    int fd;

    if((fd = open(filename, O_WRONLY | O_CREAT, 0644)) < 0)
        diep(filename);

    return fd;
}

static void logs_append(int fd, char *line) {
    if(write(fd, line, strlen(line)) < 0)
        perror("[-] logs write");

    if(write(fd, "\n", 1) < 0)
        perror("[-] logs write");
}

//
// main worker
//
static int gpsclient(settings_t *settings) {
    int fd, logsfd;
    char buffer[2048];
    char *response = NULL;
    bundle_t bundle;

    // empty bundle buffer
    bundle_init(&bundle);

    // local logs
    if(settings->logfile) {
        printf("[+] opening local log file\n");
        logsfd = logs_create(settings->logfile);
    }

    // connecting to the network
    printf("[+] validating remote server\n");
    validate(settings, "/api/ping");

    // setting up serial console
    printf("[+] opening serial device: %s\n", settings->device);
    fd = serialfd(settings->device, settings->bauds);

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

        // saving to local logs
        if(settings->logfile)
            logs_append(logsfd, buffer);

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
        .logfile = NULL
    };

    if(argc < 2) {
        fprintf(stderr, "[-] missing password\n");
        return 1;
    }

    //
    // FIXME: argument parser
    //
    char filename[256];
    int logindex = logs_index("/mnt/backlog");
    sprintf(filename, "/mnt/backlog/gps-%d", logindex);

    settings.server = "gps.maxux.net";
    settings.logfile = filename;
    settings.password = argv[1];

    return gpsclient(&settings);
}
