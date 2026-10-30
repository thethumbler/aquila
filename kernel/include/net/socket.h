#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <core/system.h>
#include <fs/vfs.h>
#include <fs/iobuf.h>

#include <aquila/socket.h>

/*
 * Sockets (net/socket.c): what every kind of socket provides, and the
 * operations that the system calls use, whatever the socket's domain.
 */

/** A socket. */
struct socket {
    int domain;
    int type;
    int protocol;

    /* its domain's operations */
    struct sock_ops *ops;

    /* the domain's own data */
    void *p;
};

/** A domain's socket operations. */
struct sock_ops {
    int (*accept) (struct file *socket, struct file *conn, const struct sockaddr *addr, socklen_t *len);
    int (*bind)   (struct file *socket, const struct sockaddr *addr, socklen_t len);
    int (*connect)(struct file *socket, const struct sockaddr *addr, socklen_t len);
    int (*listen) (struct file *socket, int backlog);

    /* recvfrom() and sendto(): `io->resid` bytes through the iobuf;
     * `from` and `to` (in kernel memory) are NULL if the caller doesn't
     * want, or doesn't give, an address */
    ssize_t (*recv)(struct file *socket, struct iobuf *io, int flags,
                    struct sockaddr *from, socklen_t *fromlen);
    ssize_t (*send)(struct file *socket, struct iobuf *io, int flags,
                    const struct sockaddr *to, socklen_t tolen);

    /* gets its address, or its peer's */
    int (*getname)(struct file *socket, struct sockaddr *addr, socklen_t *len, int peer);

    /* options of a level other than SOL_SOCKET (the protocol's), and the
     * SOL_SOCKET options that the socket layer doesn't handle itself */
    int (*getsockopt)(struct file *socket, int level, int name, void *val, socklen_t *len);
    int (*setsockopt)(struct file *socket, int level, int name, const void *val, socklen_t len);

    /* ioctl(): FIONREAD, and the protocol's own (the interfaces') */
    int (*ioctl)(struct file *socket, int request, void *argp);

    /* whether a read, or a write, wouldn't wait */
    int (*can_read)(struct file *socket, size_t len);
    int (*can_write)(struct file *socket, size_t len);

    /* the wait queue a reader (or writer) sleeps on until it can; poll()
     * waits on it too */
    struct waitq *(*wait_queue)(struct file *socket, int write);

    /* the last descriptor was closed */
    int (*shutdown) (struct file *socket, int how);

    /* shutdown(): no more reading or writing (SHUT_*); it stays open */
    int (*shut) (struct file *socket, int how);
};

/* An open file's flag: it's a socket. */
#define FILE_SOCKET     0x80000000

/* The sockets' operations, whatever their domain (net/socket.c). */
int socket_create(struct file *file, int domain, int type, int protocol);
int socket_pair(struct file *a, struct file *b, int domain, int type, int protocol);
int socket_bind(struct file *file, const struct sockaddr *addr, socklen_t len);
int socket_accept(struct file *file, struct file *conn, const struct sockaddr *addr, socklen_t *len);
int socket_connect(struct file *file, const struct sockaddr *addr, socklen_t len);
int socket_listen(struct file *file, int backlog);
ssize_t socket_send(struct file *file, struct iobuf *io, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t socket_recv(struct file *file, struct iobuf *io, int flags, struct sockaddr *from, socklen_t *fromlen);
int socket_getname(struct file *file, struct sockaddr *addr, socklen_t *len, int peer);
int socket_getsockopt(struct file *file, int level, int name, void *val, socklen_t *len);
int socket_setsockopt(struct file *file, int level, int name, const void *val, socklen_t len);
int socket_ioctl(struct file *file, int request, void *argp);
int socket_can_read(struct file *file, size_t len);
int socket_can_write(struct file *file, size_t len);
struct waitq *socket_wait_queue(struct file *file, int write);
int socket_shutdown(struct file *file, int how);
int socket_shutdown_dir(struct file *file, int how);

/* AF_UNIX */
int socket_unix_create(struct file *file, int domain, int type, int protocol);
int socket_unix_pair(struct file *a, struct file *b, int type, int protocol);

/* AF_INET */
int socket_inet_create(struct file *file, int domain, int type, int protocol);
int socket_tcp_create(struct file *file, int domain, int type, int protocol);

#endif /* ! _NET_SOCKET_H */
