/** \file
 * System calls on sockets: making them, binding, listening, accepting and
 * connecting, sending and receiving, their names and options. The work is
 * done by `net/socket.c` and the domains' code; these calls copy the
 * arguments in and the results out.
 */

#include "syscalls.h"

/**
 * Implements `socket()`: makes a socket, open on a new descriptor.
 *
 * @param domain    `AF_UNIX` or `AF_INET`
 * @param type      `SOCK_STREAM` or `SOCK_DGRAM`
 * @param protocol  the protocol, or 0 for the type's default
 * @return the descriptor, or a negative errno: `-EMFILE`, `-ENOMEM`,
 *         `-EAFNOSUPPORT`, `-EPROTONOSUPPORT`
 */
long sys_socket(int domain, int type, int protocol)
{
    syscall_log(LOG_DEBUG, "socket(domain=%d, type=%d, protocol=%d)\n",
            domain, type, protocol);

    int fd = proc_fd_get(curproc);

    if (fd == -1) {     /* every descriptor is in use */
        return -EMFILE;
    }
    
    int err = 0;
    struct file *file = file_new();

    if (!file || (err = socket_create(file, domain, type, protocol))) {
        /* the file was never opened: there's nothing to close */
        kfree(file);
        proc_fd_release(curproc, fd);
        return file ? err : -ENOMEM;
    }

    proc_fd_install(curproc, fd, file, 0);
    return fd;
}

/*
 * Socket addresses. The kernel works on its own copy, as big as the
 * biggest address (AF_UNIX's). An address coming in is `len` bytes of user
 * memory. An address going out is the protocol's, `klen` bytes, of which
 * as much as fits in the user's buffer (`*ulen` bytes) is copied out; then
 * `*ulen` is set to its full size.
 */
union ksockaddr {
    struct sockaddr sa;
    struct sockaddr_un un;
};

/** Copies an address of `len` bytes from user memory. Returns 0, `-EINVAL`
 * if it's too big, or `-EFAULT`. */
static int sockaddr_copyin(union ksockaddr *k, const struct sockaddr *uaddr, socklen_t len)
{
    if (len > sizeof(*k))
        return -EINVAL;

    return copyin(k, uaddr, len);
}

/** Reads how much room the user's buffer has for an address going out,
 * into `*klen`, at most the kernel copy's size. Returns 0 or `-EFAULT`. */
static int sockaddr_room(socklen_t *ulen, socklen_t *klen)
{
    if (copyin(klen, ulen, sizeof(*klen)))
        return -EFAULT;

    *klen = MIN(*klen, sizeof(union ksockaddr));
    return 0;
}

/** Copies an address out to user memory, as much of it as fits in `room`,
 * and sets `*ulen` to its full size `klen`. Returns 0 or `-EFAULT`. */
static int sockaddr_copyout(struct sockaddr *uaddr, socklen_t *ulen, const union ksockaddr *k, socklen_t klen, socklen_t room)
{
    if (copyout(uaddr, k, MIN(klen, room)) || copyout(ulen, &klen, sizeof(klen)))
        return -EFAULT;

    return 0;
}

/**
 * Implements `accept()`: takes a connection from a listening socket, open
 * on a new descriptor.
 *
 * @param fd    the listening socket
 * @param addr  where to put the peer's address, or NULL
 * @param len   the room at `addr`, set to the address's size
 * @return the connection's descriptor, or a negative errno: `-EBADF`,
 *         `-EMFILE`, `-ENOMEM`, `-EAGAIN`, `-EINVAL`, `-EFAULT`
 */
long sys_accept(int fd, const struct sockaddr *addr, socklen_t *len)
{
    syscall_log(LOG_DEBUG, "accept(fd=%d, addr=%p, len=%p)\n",
            fd, addr, len);

    struct file *socket = fd_file(fd);

    if (!socket) {
        return -EBADF;
    }

    int conn_fd = proc_fd_get(curproc);

    if (conn_fd == -1) {
        return -EMFILE;
    }

    struct file *conn = file_new();

    if (!conn) {
        proc_fd_release(curproc, conn_fd);
        return -ENOMEM;
    }

    union ksockaddr k;
    memset(&k, 0, sizeof(k));
    socklen_t room = 0, klen = 0;
    int err = 0;

    if (addr && (err = sockaddr_room(len, &room))) {
        kfree(conn);
        proc_fd_release(curproc, conn_fd);
        return err;
    }

    klen = room;

    if ((err = socket_accept(socket, conn, addr ? &k.sa : NULL, addr ? &klen : NULL))) {
        kfree(conn);
        proc_fd_release(curproc, conn_fd);
        return err; 
    }

    proc_fd_install(curproc, conn_fd, conn, 0);

    /* if the peer's address can't be written out, the connection is
     * closed */
    if (addr && sockaddr_copyout((struct sockaddr *) addr, len, &k, klen, room)) {
        proc_fd_close(curproc, conn_fd);
        return -EFAULT;
    }

    return conn_fd;
}

/**
 * Implements `bind()`: gives a socket its local address.
 *
 * @param fd    the socket
 * @param addr  the address
 * @param len   its size
 * @return 0, or a negative errno: `-EBADF`, `-EADDRINUSE`,
 *         `-EADDRNOTAVAIL`, `-EACCES`, `-EINVAL`, `-EFAULT`
 */
long sys_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
    syscall_log(LOG_DEBUG, "bind(fd=%d, addr=%p, len=%d)\n",
            fd, addr, len);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF; 
    }

    union ksockaddr k;
    memset(&k, 0, sizeof(k));
    int err = 0;

    if ((err = sockaddr_copyin(&k, addr, len)) || (err = socket_bind(file, &k.sa, len))) {
        return err; 
    }

    return 0;
}

/**
 * Implements `connect()`: connects a socket to a peer.
 *
 * @param fd    the socket
 * @param addr  the peer's address
 * @param len   its size
 * @return 0, or a negative errno: `-EBADF`, `-ECONNREFUSED`,
 *         `-EINPROGRESS`, `-ENETUNREACH`, `-ETIMEDOUT`, `-EFAULT`
 */
long sys_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
    syscall_log(LOG_DEBUG, "connect(fd=%d, addr=%p, len=%d)\n",
            fd, addr, len);

    struct file *socket = fd_file(fd);

    if (!socket) {
        return -EBADF; 
    }

    union ksockaddr k;
    memset(&k, 0, sizeof(k));
    int err = 0;

    if ((err = sockaddr_copyin(&k, addr, len)) || (err = socket_connect(socket, &k.sa, len))) {
        return err; 
    }

    return 0;
}

/**
 * Implements `listen()`: makes a socket accept connections.
 *
 * @param fd       the socket
 * @param backlog  how many connections may wait to be accepted
 * @return 0, or a negative errno: `-EBADF`, `-EINVAL`, `-EOPNOTSUPP`
 */
long sys_listen(int fd, int backlog)
{
    syscall_log(LOG_DEBUG, "listen(fd=%d, backlog=%d)\n", fd, backlog);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF; 
    }

    int err = 0;

    if ((err = socket_listen(file, backlog))) {
        return err; 
    }

    return 0;
}

/**
 * Implements `send()` and `sendto()`. A send on a connection that can't
 * send anymore also raises `SIGPIPE`, unless `MSG_NOSIGNAL` is set.
 *
 * @param us  the arguments, in user memory: the socket, the data, the
 *            flags, and `sendto()`'s destination
 * @return how many bytes were sent, or a negative errno: `-EBADF`,
 *         `-EFAULT`, `-EPIPE`, `-EAGAIN`, `-EMSGSIZE`, ...
 */
long sys_send(struct aquila_socket_io_args *us)
{
    struct aquila_socket_io_args _s, *s = &_s;

    if (copyin(s, us, sizeof(*s))) {
        return -EFAULT;
    }

    int fd = s->fd;
    void *buf = s->buf;
    size_t len = s->len;
    int flags = s->flags;

    syscall_log(LOG_DEBUG, "send(fd=%d, buf=%p, len=%d, flags=%x)\n",
            fd, buf, len, flags);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF; 
    }

    int err = 0;

    if (uaccess_check(buf, len)) {
        return -EFAULT;
    }

    /* sendto(): the destination */
    union ksockaddr to;
    memset(&to, 0, sizeof(to));
    socklen_t tolen = 0;

    if (s->addr) {
        if (copyin(&tolen, s->addrlen, sizeof(tolen))) {
            return -EFAULT;
        }

        if ((err = sockaddr_copyin(&to, s->addr, tolen))) {
            return err;
        }
    }

    struct iobuf io;
    iobuf_init(&io, buf, len, 1);

    ssize_t ret = socket_send(file, &io, flags, s->addr ? &to.sa : NULL, tolen);

    /* a connection that can't send anymore raises SIGPIPE too, as write()
     * does (not with MSG_NOSIGNAL) */
    if (ret == -EPIPE && !(flags & MSG_NOSIGNAL))
        signal_thread_send(curthread, SIGPIPE);

    return ret;
}

/**
 * Implements `recv()` and `recvfrom()`.
 *
 * @param us  the arguments, in user memory: the socket, the buffer, the
 *            flags, and where `recvfrom()` puts the sender's address
 * @return how many bytes were received (0 at the end), or a negative
 *         errno: `-EBADF`, `-EFAULT`, `-EAGAIN`, `-ENOTCONN`, ...
 */
long sys_recv(struct aquila_socket_io_args *us)
{
    struct aquila_socket_io_args _s, *s = &_s;

    if (copyin(s, us, sizeof(*s))) {
        return -EFAULT;
    }

    int fd = s->fd;
    void *buf = s->buf;
    size_t len = s->len;
    int flags = s->flags;

    syscall_log(LOG_DEBUG, "recv(fd=%d, buf=%p, len=%d, flags=%x)\n",
            fd, buf, len, flags);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF; 
    }

    int err = 0;

    if (uaccess_check(buf, len)) {
        return -EFAULT;
    }

    /* recvfrom(): the sender's address */
    union ksockaddr from;
    memset(&from, 0, sizeof(from));
    socklen_t room = 0, fromlen = 0;

    if (s->addr && (err = sockaddr_room(s->addrlen, &room))) {
        return err;
    }

    fromlen = room;

    struct iobuf io;
    iobuf_init(&io, buf, len, 1);

    ssize_t ret = socket_recv(file, &io, flags, s->addr ? &from.sa : NULL, s->addr ? &fromlen : NULL);

    if (ret >= 0 && s->addr && sockaddr_copyout(s->addr, s->addrlen, &from, fromlen, room))
        ret = -EFAULT;

    return ret;
}

/** Gets a socket's local address or its peer's, for `getsockname()` and
 * `getpeername()`, and copies it out. */
static long socket_name(int fd, struct sockaddr *addr, socklen_t *len, int peer)
{
    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF;
    }

    union ksockaddr k;
    memset(&k, 0, sizeof(k));
    socklen_t room, klen;
    int err;

    if ((err = sockaddr_room(len, &room))) {
        return err;
    }

    klen = room;

    if (!(err = socket_getname(file, &k.sa, &klen, peer)))
        err = sockaddr_copyout(addr, len, &k, klen, room);

    return err;
}

/**
 * Implements `getsockname()`: a socket's local address.
 *
 * @param fd    the socket
 * @param addr  where to put the address
 * @param len   the room at `addr`, set to the address's size
 * @return 0, or a negative errno: `-EBADF`, `-ENOTSOCK`, `-EFAULT`
 */
long sys_getsockname(int fd, struct sockaddr *addr, socklen_t *len)
{
    syscall_log(LOG_DEBUG, "getsockname(fd=%d, addr=%p, len=%p)\n", fd, addr, len);
    return socket_name(fd, addr, len, 0);
}

/**
 * Implements `getpeername()`: the address of a socket's peer.
 *
 * @param fd    the socket
 * @param addr  where to put the address
 * @param len   the room at `addr`, set to the address's size
 * @return 0, or a negative errno: `-EBADF`, `-ENOTCONN`, `-EFAULT`
 */
long sys_getpeername(int fd, struct sockaddr *addr, socklen_t *len)
{
    syscall_log(LOG_DEBUG, "getpeername(fd=%d, addr=%p, len=%p)\n", fd, addr, len);
    return socket_name(fd, addr, len, 1);
}

/**
 * Implements `getsockopt()`: reads a socket option.
 *
 * @param fd     the socket
 * @param level  `SOL_SOCKET`, or a protocol's level
 * @param name   the option
 * @param val    where to put its value
 * @param len    the room at `val`, set to the value's size
 * @return 0, or a negative errno: `-EBADF`, `-ENOPROTOOPT`, `-EINVAL`,
 *         `-EFAULT`
 */
long sys_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
    syscall_log(LOG_DEBUG, "getsockopt(fd=%d, level=%d, name=%d, val=%p, len=%p)\n",
            fd, level, name, val, len);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF;
    }

    /* the kernel's copy of the value (the biggest is a struct timeval) */
    char kval[64];
    memset(kval, 0, sizeof(kval));
    socklen_t room, klen;
    int err;

    if (copyin(&room, len, sizeof(room))) {
        return -EFAULT;
    }

    klen = room = MIN(room, sizeof(kval));

    if (!(err = socket_getsockopt(file, level, name, kval, &klen)) &&
        (copyout(val, kval, MIN(klen, room)) || copyout(len, &klen, sizeof(klen))))
        err = -EFAULT;

    return err;
}

/**
 * Implements `setsockopt()`: sets a socket option.
 *
 * @param fd     the socket
 * @param level  `SOL_SOCKET`, or a protocol's level
 * @param name   the option
 * @param val    its value
 * @param len    the value's size
 * @return 0, or a negative errno: `-EBADF`, `-ENOPROTOOPT`, `-EINVAL`,
 *         `-EPERM`, `-EFAULT`
 */
long sys_setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
    syscall_log(LOG_DEBUG, "setsockopt(fd=%d, level=%d, name=%d, val=%p, len=%d)\n",
            fd, level, name, val, len);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF;
    }

    char kval[64];

    len = MIN(len, sizeof(kval));

    if (copyin(kval, val, len)) {
        return -EFAULT;
    }

    return socket_setsockopt(file, level, name, kval, len);
}

/**
 * Implements `shutdown()`: shuts down reading, writing or both on a
 * socket, which stays open.
 *
 * @param fd   the socket
 * @param how  `SHUT_RD`, `SHUT_WR` or `SHUT_RDWR`
 * @return 0, or a negative errno: `-EBADF`, `-ENOTSOCK`, `-ENOTCONN`,
 *         `-EINVAL`
 */
long sys_shutdown(int fd, int how)
{
    syscall_log(LOG_DEBUG, "shutdown(fd=%d, how=%d)\n", fd, how);

    struct file *file = fd_file(fd);

    if (!file) {
        return -EBADF;
    }

    return socket_shutdown_dir(file, how);
}
