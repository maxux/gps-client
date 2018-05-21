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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

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
    char *pusher;     // push pipe filename

} settings_t;

//
// error handling
//
static void diep(char *str) {
    fprintf(stderr, "[-] %s: [%d] %s\n", str, errno, strerror(errno));
    exit(EXIT_FAILURE);
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

    sprintf(buffer, "%05d", value);

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

static int push_init(char *filename) {
    int fd;

    if((fd = open(filename, O_WRONLY | O_NONBLOCK)) < 0) {
        if(errno == ENXIO) {
            printf("[-] push: end-pipe not ready yet\n");
            return -1;
        }

        diep(filename);
    }

    if(fcntl(fd, F_SETPIPE_SZ, 32 * 1024 * 1024) < 1)
        diep("fcntl");

    printf("[+] push: pipe opened\n");

    return fd;
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
static int gpsgateway(settings_t *settings) {
    int fd, logsfd, pushfd;
    char buffer[2048];
    pid_t child;

    // local logs
    if(settings->logfile) {
        printf("[+] opening local log file\n");
        logsfd = logs_create(settings->logfile);
    }

    // ignoring broken pipe
    signal(SIGPIPE, SIG_IGN);

    // gateway pipe filename
    printf("[+] creating fifo push file\n");
    if(mkfifo(settings->pusher, 0644))
        diep("mkfifo");

    // assume fifo cannot be read now, since if was
    // just created
    pushfd = -1;

    // setting up serial console
    printf("[+] opening serial device: %s\n", settings->device);
    fd = serialfd(settings->device, settings->bauds);

    // we are ready now, let's fork
    if((child = fork()) < 0)
        diep("fork");

    if(child)
        exit(EXIT_SUCCESS);

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

        // pushing data
        if(pushfd < 0) {
            printf("[+] push not ready, trying to open it\n");
            pushfd = push_init(settings->pusher);
        }

        if(pushfd >= 0) {
            printf("[+] pushing gps line\n");
            logs_append(pushfd, buffer);
        }
    }

    return 0;
}

int main(void) {
    settings_t settings = {
        .device = "/dev/ttyAMA0",
        .bauds = B9600,
        .server = NULL,
        .port = 80,
        .password = "",
        .logfile = NULL,
        .pusher = "/tmp/gps.pipe",
    };

    //
    // FIXME: argument parser
    //
    char filename[256];
    int logindex = logs_index("/mnt/backlog");
    sprintf(filename, "/mnt/backlog/gps-%05d", logindex);

    settings.server = "gps.maxux.net";
    settings.logfile = filename;

    return gpsgateway(&settings);
}
