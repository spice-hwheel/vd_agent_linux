/*  udscs.c Unix Domain Socket Client Server framework. A framework for quickly
    creating select() based servers capable of handling multiple clients and
    matching select() based clients using variable size messages.

    Copyright 2010 Red Hat, Inc.

    Red Hat Authors:
    Hans de Goede <hdegoede@redhat.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or   
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of 
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the  
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "udscs.h"

struct udscs_buf {
    uint8_t *buf;
    size_t pos;
    size_t size;
    
    struct udscs_buf *next;
};

struct udscs_connection {
    int fd;
    const char * const *type_to_string;
    int no_types;
    FILE *logfile;
    FILE *errfile;

    /* Read stuff, single buffer, separate header and data buffer */
    int header_read;
    struct udscs_message_header header;
    struct udscs_buf data;

    /* Writes are stored in a linked list of buffers, with both the header
       + data for a single message in 1 buffer. */
    struct udscs_buf *write_buf;

    /* Callbacks */
    udscs_read_callback read_callback;
    udscs_disconnect_callback disconnect_callback;

    struct udscs_connection *next;
    struct udscs_connection *prev;
};

struct udscs_server {
    int fd;
    const char * const *type_to_string;
    int no_types;
    FILE *logfile;
    FILE *errfile;
    struct udscs_connection connections_head;
    udscs_connect_callback connect_callback;
    udscs_read_callback read_callback;
    udscs_disconnect_callback disconnect_callback;
};

static void udscs_do_write(struct udscs_connection **connp);
static void udscs_do_read(struct udscs_connection **connp);


struct udscs_server *udscs_create_server(const char *socketname,
    udscs_connect_callback connect_callback,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types,
    FILE *logfile, FILE *errfile)
{
    int c;
    struct sockaddr_un address;
    struct udscs_server *server;

    server = calloc(1, sizeof(*server));
    if (!server)
        return NULL;

    server->logfile = logfile;
    server->errfile = errfile;
    server->type_to_string = type_to_string;
    server->no_types = no_types;

    server->fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (server->fd == -1) {
        fprintf(server->errfile, "creating unix domain socket: %s\n",
                strerror(errno));
        free(server);
        return NULL;
    }

    c = unlink(socketname);
    if (c != 0 && errno != ENOENT) {
        fprintf(server->errfile, "unlink %s: %s\n", socketname,
                strerror(errno));
        free(server);
        return NULL;
    }

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketname);
    c = bind(server->fd, (struct sockaddr *)&address, sizeof(address));
    if (c != 0) {
        fprintf(server->errfile, "bind %s: %s\n", socketname, strerror(errno));
        free(server);
        return NULL;
    }

    c = listen(server->fd, 5);
    if (c != 0) {
        fprintf(server->errfile, "listen: %s\n", strerror(errno));
        free(server);
        return NULL;
    }

    server->connect_callback = connect_callback;
    server->read_callback = read_callback;
    server->disconnect_callback = disconnect_callback;

    return server;
}

void udscs_destroy_server(struct udscs_server *server)
{
    struct udscs_connection *conn, *next_conn;

    if (!server)
        return;

    conn = server->connections_head.next;
    while (conn) {
        next_conn = conn->next;
        udscs_destroy_connection(&conn);
        conn = next_conn;
    }
    close(server->fd);
    free(server);
}

struct udscs_connection *udscs_connect(const char *socketname,
    udscs_read_callback read_callback,
    udscs_disconnect_callback disconnect_callback,
    const char * const type_to_string[], int no_types,
    FILE *logfile, FILE *errfile)
{
    int c;
    struct sockaddr_un address;
    struct udscs_connection *conn;

    conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;

    conn->logfile = logfile;
    conn->errfile = errfile;
    conn->type_to_string = type_to_string;
    conn->no_types = no_types;

    conn->fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (conn->fd == -1) {
        fprintf(conn->errfile, "creating unix domain socket: %s\n",
                strerror(errno));
        free(conn);
        return NULL;
    }

    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketname);
    c = connect(conn->fd, (struct sockaddr *)&address, sizeof(address));
    if (c != 0) {
        fprintf(conn->errfile, "connect %s: %s\n", socketname,
                strerror(errno));
        free(conn);
        return NULL;
    }

    conn->read_callback = read_callback;
    conn->disconnect_callback = disconnect_callback;

    if (conn->logfile)
        fprintf(conn->logfile, "%p connected to %s\n", conn, socketname);

    return conn;
}

void udscs_destroy_connection(struct udscs_connection **connp)
{
    struct udscs_buf *wbuf, *next_wbuf;
    struct udscs_connection *conn = *connp;

    if (!conn)
        return;

    if (conn->disconnect_callback)
        conn->disconnect_callback(conn);

    wbuf = conn->write_buf;
    while (wbuf) {
        next_wbuf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
        wbuf = next_wbuf;
    }

    free(conn->data.buf);

    if (conn->prev)
        conn->prev->next = conn->next;

    close(conn->fd);

    if (conn->logfile)
        fprintf(conn->logfile, "%p disconnected\n", conn);

    free(conn);
    *connp = NULL;
}

int udscs_server_fill_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds)
{
    struct udscs_connection *conn;
    int nfds = server->fd + 1;

    if (!server)
        return -1;

    FD_SET(server->fd, readfds);

    conn = server->connections_head.next;
    while (conn) {
        int conn_nfds = udscs_client_fill_fds(conn, readfds, writefds);
        if (conn_nfds > nfds)
            nfds = conn_nfds;

        conn = conn->next;
    }

    return nfds;
}

int udscs_client_fill_fds(struct udscs_connection *conn, fd_set *readfds,
        fd_set *writefds)
{
    if (!conn)
        return -1;

    FD_SET(conn->fd, readfds);
    if (conn->write_buf)
        FD_SET(conn->fd, writefds);

    return conn->fd + 1;
}

static void udscs_server_accept(struct udscs_server *server) {
    struct udscs_connection *new_conn, *conn;
    struct sockaddr_un address;
    socklen_t address_length = sizeof(address);
    int fd;

    fd = accept(server->fd, (struct sockaddr *)&address, &address_length);
    if (fd == -1) {
        if (errno == EINTR)
            return;
        fprintf(server->errfile, "accept: %s\n", strerror(errno));
        return;
    }

    new_conn = calloc(1, sizeof(*conn));
    if (!new_conn) {
        fprintf(server->errfile, "out of memory, disconnecting new client\n");
        close(fd);
        return;
    }

    new_conn->fd = fd;
    new_conn->logfile = server->logfile;
    new_conn->errfile = server->errfile;
    new_conn->type_to_string = server->type_to_string;
    new_conn->no_types = server->no_types;
    new_conn->read_callback = server->read_callback;
    new_conn->disconnect_callback = server->disconnect_callback;

    conn = &server->connections_head;
    while (conn->next)
        conn = conn->next;

    new_conn->prev = conn;
    conn->next = new_conn;

    if (server->logfile)
        fprintf(server->logfile, "new client accepted: %p\n", new_conn);

    if (server->connect_callback)
        server->connect_callback(new_conn);
}

void udscs_server_handle_fds(struct udscs_server *server, fd_set *readfds,
        fd_set *writefds)
{
    struct udscs_connection *conn, *next_conn;

    if (!server)
        return;

    if (FD_ISSET(server->fd, readfds))
        udscs_server_accept(server);

    conn = server->connections_head.next;
    while (conn) {
        /* conn maybe destroyed by udscs_client_handle_fds (when disconnected),
           so get the next connection first. */
        next_conn = conn->next;
        udscs_client_handle_fds(&conn, readfds, writefds);
        conn = next_conn;
    }
}

void udscs_client_handle_fds(struct udscs_connection **connp, fd_set *readfds,
        fd_set *writefds)
{
    if (!*connp)
        return;

    if (FD_ISSET((*connp)->fd, readfds))
        udscs_do_read(connp);

    if (*connp && FD_ISSET((*connp)->fd, writefds))
        udscs_do_write(connp);
}

int udscs_write(struct udscs_connection *conn, uint32_t type, uint32_t opaque,
        const uint8_t *data, uint32_t size)
{
    struct udscs_buf *wbuf, *new_wbuf;
    struct udscs_message_header header;

    new_wbuf = malloc(sizeof(*new_wbuf));
    if (!new_wbuf)
        return -1;

    new_wbuf->pos = 0;
    new_wbuf->size = sizeof(header) + size;
    new_wbuf->next = NULL;
    new_wbuf->buf = malloc(new_wbuf->size);
    if (!new_wbuf->buf) {
        free(new_wbuf);
        return -1;
    }

    header.type = type;
    header.opaque = opaque;
    header.size = size;

    memcpy(new_wbuf->buf, &header, sizeof(header));
    memcpy(new_wbuf->buf + sizeof(header), data, size);

    if (conn->logfile) {
        if (type < conn->no_types)
            fprintf(conn->logfile, "%p sent %s, opaque: %u, size %u\n",
                    conn, conn->type_to_string[type], opaque, size);
        else
            fprintf(conn->logfile,
                    "%p sent invalid message %u, opaque: %u, size %u\n",
                    conn, type, opaque, size);
    }

    if (!conn->write_buf) {
        conn->write_buf = new_wbuf;
        return 0;
    }

    /* FIXME maybe limit the write_buf stack depth ? */
    wbuf = conn->write_buf;
    while (wbuf->next)
        wbuf = wbuf->next;

    wbuf->next = new_wbuf;

    return 0;
}

int udscs_server_write_all(struct udscs_server *server,
        uint32_t type, uint32_t opaque,
        const uint8_t *data, uint32_t size)
{
    struct udscs_connection *conn;

    conn = server->connections_head.next;
    while (conn) {
        if (udscs_write(conn, type, opaque, data, size))
            return -1;
        conn = conn->next;
    }

    return 0;
}

static void udscs_read_complete(struct udscs_connection **connp)
{
    struct udscs_connection *conn = *connp;

    if (conn->logfile) {
        if (conn->header.type < conn->no_types)
            fprintf(conn->logfile, "%p received %s, opaque: %u, size %u\n",
                    conn, conn->type_to_string[conn->header.type],
                    conn->header.opaque, conn->header.size);
        else
            fprintf(conn->logfile,
                    "%p received invalid message %u, opaque: %u, size %u\n",
                    conn, conn->header.type, conn->header.opaque,
                    conn->header.size);
    }

    if (conn->read_callback) {
        if (conn->read_callback(conn, &conn->header, conn->data.buf) == -1) {
            udscs_destroy_connection(connp);
            return;
        }
    }

    free(conn->data.buf);
    conn->header_read = 0;
    memset(&conn->data, 0, sizeof(conn->data));
}

static void udscs_do_read(struct udscs_connection **connp)
{
    ssize_t n;
    size_t to_read;
    uint8_t *dest;
    struct udscs_connection *conn = *connp;

    if (conn->header_read < sizeof(conn->header)) {
        to_read = sizeof(conn->header) - conn->header_read;
        dest = (uint8_t *)&conn->header + conn->header_read;
    } else {
        to_read = conn->data.size - conn->data.pos;
        dest = conn->data.buf + conn->data.pos;
    }

    n = read(conn->fd, dest, to_read);
    if (n < 0) {
        if (errno == EINTR)
            return;
        fprintf(conn->errfile,
                "reading unix domain socket: %s, disconnecting %p\n",
                strerror(errno), conn);
    }
    if (n <= 0) {
        udscs_destroy_connection(connp);
        return;
    }

    if (conn->header_read < sizeof(conn->header)) {
        conn->header_read += n;
        if (conn->header_read == sizeof(conn->header)) {
            if (conn->header.size == 0) {
                udscs_read_complete(connp);
                return;
            }
            conn->data.pos = 0;
            conn->data.size = conn->header.size;
            conn->data.buf = malloc(conn->data.size);
            if (!conn->data.buf) {
                fprintf(conn->errfile, "out of memory, disconnecting %p\n",
                        conn);
                udscs_destroy_connection(connp);
                return;
            }
        }
    } else {
        conn->data.pos += n;
        if (conn->data.pos == conn->data.size)
            udscs_read_complete(connp);
    }
}

static void udscs_do_write(struct udscs_connection **connp)
{
    ssize_t n;
    size_t to_write;
    struct udscs_connection *conn = *connp;

    struct udscs_buf* wbuf = conn->write_buf;
    if (!wbuf) {
        fprintf(conn->errfile,
                "%p do_write called on a connection without a write buf ?!\n",
                conn);
        return;
    }

    to_write = wbuf->size - wbuf->pos;
    n = write(conn->fd, wbuf->buf + wbuf->pos, to_write);
    if (n < 0) {
        if (errno == EINTR)
            return;
        fprintf(conn->errfile,
                "writing to unix domain socket: %s, disconnecting %p\n",
                strerror(errno), conn);
        udscs_destroy_connection(connp);
        return;
    }

    wbuf->pos += n;
    if (wbuf->pos == wbuf->size) {
        conn->write_buf = wbuf->next;
        free(wbuf->buf);
        free(wbuf);
    }
}
