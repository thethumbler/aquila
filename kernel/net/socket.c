/** \file
 * Sockets: what every domain's sockets share.
 *
 * A socket is an open file with `FILE_SOCKET` set. The system calls on
 * sockets come here, which checks that the file is one, then passes the
 * call to its domain's operations (`struct sock_ops`): Unix sockets
 * (`net/unix/unix.c`) or Internet ones (`net/inet/socket.c`).
 */

#include <core/system.h>
#include <core/string.h>
#include <ds/queue.h>
#include <aquila/errno.h>
#include <net/socket.h>

#include <aquila/fcntl.h>

/**
 * Makes a socket of a domain in an open file, for `socket()`.
 *
 * @param file      the open file that becomes the socket
 * @param domain    `AF_UNIX` or `AF_INET`
 * @param type      the socket's type (`SOCK_STREAM`, `SOCK_DGRAM`)
 * @param protocol  the protocol, or 0 for the type's default
 * @return 0, `-EAFNOSUPPORT` for another domain, or the domain's error
 */
int socket_create(struct file *file, int domain, int type, int protocol)
{
    int err;

    switch (domain) {
        case AF_UNIX:
            err = socket_unix_create(file, domain, type, protocol);
            break;
        case AF_INET:
            err = socket_inet_create(file, domain, type, protocol);
            break;
        default:
            return -EAFNOSUPPORT;
    }

    /* a socket is open for reading and writing (F_GETFL says so, and
     * fdopen() checks it) */
    if (!err)
        file->flags |= O_RDWR;

    return err;
}

/**
 * Makes two sockets connected to each other in two open files, for
 * `socketpair()`. Only Unix sockets can be made so.
 *
 * @param a         the open file that becomes one socket
 * @param b         the open file that becomes the other
 * @param domain    `AF_UNIX`
 * @param type      `SOCK_STREAM`
 * @param protocol  0
 * @return 0, `-EOPNOTSUPP` for `AF_INET`, `-EAFNOSUPPORT` for another
 *         domain, or the domain's error
 */
int socket_pair(struct file *a, struct file *b, int domain, int type, int protocol)
{
    int err;

    switch (domain) {
        case AF_UNIX:
            err = socket_unix_pair(a, b, type, protocol);
            break;
        case AF_INET:
            return -EOPNOTSUPP;
        default:
            return -EAFNOSUPPORT;
    }

    if (!err) {
        a->flags |= O_RDWR;
        b->flags |= O_RDWR;
    }

    return err;
}

/**
 * Accepts a connection on a listening socket, for `accept()`.
 *
 * @param file  the listening socket
 * @param conn  the open file that becomes the connection's socket
 * @param addr  where to put the peer's address, in user memory, or NULL
 * @param len   its size, in user memory
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_accept(struct file *file, struct file *conn, const struct sockaddr *addr, socklen_t *len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->accept)
        return -EOPNOTSUPP;

    int err = file->socket->ops->accept(file, conn, addr, len);

    if (!err)
        conn->flags |= O_RDWR;

    return err;
}

/**
 * Gives a socket its local address, for `bind()`.
 *
 * @param file  the socket
 * @param addr  the address, in the kernel's memory
 * @param len   its size
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_bind(struct file *file, const struct sockaddr *addr, socklen_t len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->bind)
        return -EOPNOTSUPP;

    return file->socket->ops->bind(file, addr, len);
}

/**
 * Connects a socket to a peer, for `connect()`.
 *
 * @param file  the socket
 * @param addr  the peer's address, in the kernel's memory
 * @param len   its size
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_connect(struct file *file, const struct sockaddr *addr, socklen_t len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->connect)
        return -EOPNOTSUPP;

    return file->socket->ops->connect(file, addr, len);
}

/**
 * Makes a socket listen for connections, for `listen()`.
 *
 * @param file     the socket
 * @param backlog  how many connections may wait to be accepted
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_listen(struct file *file, int backlog)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->listen)
        return -EOPNOTSUPP;

    return file->socket->ops->listen(file, backlog);
}

/**
 * Sends data on a socket, for `send()`, `sendto()` and `write()`.
 *
 * @param file   the socket
 * @param io     the data, in user memory
 * @param flags  `MSG_*` flags
 * @param to     the destination, for a datagram socket, or NULL
 * @param tolen  its size
 * @return how many bytes were sent, or a negative errno
 */
ssize_t socket_send(struct file *file, struct iobuf *io, int flags, const struct sockaddr *to, socklen_t tolen)
{
    if (!file)
        return -EINVAL;

    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!(file->socket))
        return -EINVAL;

    if (!file->socket->ops || !file->socket->ops->send)
        return -EOPNOTSUPP;

    return file->socket->ops->send(file, io, flags, to, tolen);
}

/**
 * Receives data from a socket, for `recv()`, `recvfrom()` and `read()`.
 *
 * @param file     the socket
 * @param io       where to put the data, in user memory
 * @param flags    `MSG_*` flags
 * @param from     where to put the sender's address, or NULL
 * @param fromlen  its size
 * @return how many bytes were received (0 at the end), or a negative
 *         errno
 */
ssize_t socket_recv(struct file *file, struct iobuf *io, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->recv)
        return -EOPNOTSUPP;

    return file->socket->ops->recv(file, io, flags, from, fromlen);
}

/**
 * Says whether a socket has something to read, for `select()` and
 * `poll()`.
 *
 * @param file  the socket
 * @param len   how much a read would want
 * @return non-zero if a read wouldn't wait, 0 if it would, or a negative
 *         errno
 */
int socket_can_read(struct file *file, size_t len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->can_read)
        return -EOPNOTSUPP;

    return file->socket->ops->can_read(file, len);
}

/**
 * Finds the wait queue that a socket's readers (or writers) sleep on, for
 * `select()` and `poll()` to register.
 *
 * @param file   the socket
 * @param write  0 for readers, 1 for writers
 * @return the wait queue, or NULL if the protocol can't say, in which
 *         case any wakeup counts
 */
struct waitq *socket_wait_queue(struct file *file, int write)
{
    if (!file->socket->ops || !file->socket->ops->wait_queue)
        return NULL;

    return file->socket->ops->wait_queue(file, write);
}

/**
 * Says whether a socket has room to write, for `select()` and `poll()`.
 *
 * @param file  the socket
 * @param len   how much a write would want
 * @return non-zero if a write wouldn't wait, 0 if it would, or a negative
 *         errno
 */
int socket_can_write(struct file *file, size_t len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->can_write)
        return -EOPNOTSUPP;

    return file->socket->ops->can_write(file, len);
}

/**
 * Shuts down reading, writing or both on a socket, for `shutdown()`. The
 * socket stays open.
 *
 * @param file  the socket
 * @param how   `SHUT_RD`, `SHUT_WR` or `SHUT_RDWR`
 * @return 0, `-EINVAL` for a bad `how`, `-ENOTSOCK`, `-EOPNOTSUPP`, or
 *         the protocol's error
 */
int socket_shutdown_dir(struct file *file, int how)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (how & ~SHUT_RDWR || !how)
        return -EINVAL;

    if (!file->socket->ops || !file->socket->ops->shut)
        return -EOPNOTSUPP;

    return file->socket->ops->shut(file, how);
}

/**
 * Releases a socket when its open file is closed (its last descriptor
 * went).
 *
 * @param file  the socket
 * @param how   passed on to the protocol
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_shutdown(struct file *file, int how)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->shutdown)
        return -EOPNOTSUPP;

    /* the open file is closed (its last descriptor went) */
    return file->socket->ops->shutdown(file, how);
}

/**
 * Gets a socket's local address, or its peer's, for `getsockname()` and
 * `getpeername()`.
 *
 * @param file  the socket
 * @param addr  where to put the address, in the kernel's memory
 * @param len   its size, set to the address's
 * @param peer  0 for the local address, 1 for the peer's
 * @return 0, `-ENOTSOCK`, `-EOPNOTSUPP`, or the protocol's error
 */
int socket_getname(struct file *file, struct sockaddr *addr, socklen_t *len, int peer)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->getname)
        return -EOPNOTSUPP;

    return file->socket->ops->getname(file, addr, len, peer);
}

/** Sets an option's value to the int `v`. Returns 0, or `-EINVAL` if
 * `*len` has no room for an int. */
static int sockopt_int(void *val, socklen_t *len, int v)
{
    if (*len < sizeof(int))
        return -EINVAL;

    *(int *) val = v;
    *len = sizeof(int);
    return 0;
}

/**
 * Reads a socket option, for `getsockopt()`. The protocol is asked first;
 * `SO_TYPE`, and `SO_ERROR` for a protocol that keeps no errors, are
 * answered here.
 *
 * @param file   the socket
 * @param level  `SOL_SOCKET`, or a protocol's level
 * @param name   the option
 * @param val    where to put its value, in the kernel's memory
 * @param len    its size, set to the value's
 * @return 0, `-ENOPROTOOPT` for an unknown option, or another negative
 *         errno
 */
int socket_getsockopt(struct file *file, int level, int name, void *val, socklen_t *len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    struct sock_ops *ops = file->socket->ops;

    if (level == SOL_SOCKET && name == SO_TYPE)
        return sockopt_int(val, len, file->socket->type);

    if (ops && ops->getsockopt) {
        int err = ops->getsockopt(file, level, name, val, len);

        if (err != -ENOPROTOOPT)
            return err;
    }

    /* if the protocol doesn't keep errors, there is no pending one */
    if (level == SOL_SOCKET && name == SO_ERROR)
        return sockopt_int(val, len, 0);

    return -ENOPROTOOPT;
}

/**
 * Sets a socket option, for `setsockopt()`. The protocol is asked first;
 * a few common `SOL_SOCKET` options are accepted here without effect.
 *
 * @param file   the socket
 * @param level  `SOL_SOCKET`, or a protocol's level
 * @param name   the option
 * @param val    its value, in the kernel's memory
 * @param len    its size
 * @return 0, `-ENOPROTOOPT` for an unknown option, or another negative
 *         errno
 */
int socket_setsockopt(struct file *file, int level, int name, const void *val, socklen_t len)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    struct sock_ops *ops = file->socket->ops;

    if (ops && ops->setsockopt) {
        int err = ops->setsockopt(file, level, name, val, len);

        if (err != -ENOPROTOOPT)
            return err;
    }

    /* these are accepted, without effect */
    if (level == SOL_SOCKET) {
        switch (name) {
            case SO_REUSEADDR:
            case SO_KEEPALIVE:
            case SO_SNDBUF:
            case SO_RCVBUF:
            case SO_LINGER:
            case SO_SNDTIMEO:
                return 0;
        }
    }

    return -ENOPROTOOPT;
}

/**
 * Handles an `ioctl()` on a socket, by passing it to the protocol.
 *
 * @param file     the socket
 * @param request  the request
 * @param argp     its argument, in user memory
 * @return what the protocol returns, or `-ENOTTY` if it has no `ioctl()`
 */
int socket_ioctl(struct file *file, int request, void *argp)
{
    if (!(file->flags & FILE_SOCKET))
        return -ENOTSOCK;

    if (!file->socket->ops || !file->socket->ops->ioctl)
        return -ENOTTY;

    return file->socket->ops->ioctl(file, request, argp);
}
