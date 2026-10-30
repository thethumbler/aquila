/** \file
 * Unix domain sockets (`AF_UNIX`): stream sockets between processes of
 * this machine, named by a path in the file system.
 *
 * A server binds a socket to a path, which makes a socket node there, and
 * listens. A client connects to that path: its request waits on the
 * listening socket until `accept()` takes it. The connection has a ring
 * buffer each way, and each side sleeps on its own wait queues when a
 * ring is empty or full. On a connection's open files, `offset` says the
 * side: 0 for the server, 1 for the client. Only `SOCK_STREAM` is
 * supported.
 */

#include <core/system.h>
#include <aquila/ioctl.h>
#include <sys/sched.h>
#include <fs/vfs.h>
#include <ds/ringbuf.h>
#include <net/socket.h>
#include <sys/uaccess.h>
#include <unix.h>

#include <aquila/fcntl.h>

/* How much a connection holds each way. X servers send replies of
 * hundreds of KB (font metrics), and every time the ring fills, the
 * processes have to switch, so the rings are big. */
#define SOCKBUF 65536

MALLOC_DEFINE(M_SOCKET, "socket", "socket struct");
MALLOC_DEFINE(M_UN_SOCKET, "unix socket", "unix socket struct");
MALLOC_DEFINE(M_UN_CONN, "unix conn", "unix connection struct");

static struct sock_ops socket_unix_ops;

static struct queue *sockets = QUEUE_NEW();  /* the bound sockets */

/** Checks an `AF_UNIX` address (the kernel's copy, as big as a
 * `struct sockaddr_un`): its path must end within it. Returns 0 or
 * `-EINVAL`. */
static int unix_addr_check(const struct sockaddr *addr)
{
    struct sockaddr_un *addr_un = (struct sockaddr_un *) addr;

    return strnlen(addr_un->sun_path, sizeof(addr_un->sun_path)) < sizeof(addr_un->sun_path) ? 0 : -EINVAL;
}

/** Wakes up every thread sleeping on a connection, so that they notice
 * that its state changed. */
static void unix_conn_wakeup(struct un_conn *conn)
{
    waitq_wakeup(&conn->server_recv);
    waitq_wakeup(&conn->server_send);
    waitq_wakeup(&conn->client_recv);
    waitq_wakeup(&conn->client_send);
}

/**
 * Makes an `AF_UNIX` socket, for `socket()`. It is neither bound nor
 * connected yet.
 *
 * @param file      the open file that becomes the socket
 * @param domain    `AF_UNIX`
 * @param type      `SOCK_STREAM` (the only one supported)
 * @param protocol  0
 * @return 0, `-EPROTONOSUPPORT`, or `-ENOMEM`
 */
int socket_unix_create(struct file *file, int domain, int type, int protocol)
{
    /* streams only: refusing other types is better than making a stream
     * socket where a datagram socket was asked for */
    if (type != SOCK_STREAM)
        return -EPROTONOSUPPORT;

    if (protocol)
        return -EPROTONOSUPPORT;

    struct socket *socket = kmalloc(sizeof(struct socket), &M_SOCKET, M_ZERO);
    if (!socket) return -ENOMEM;

    socket->domain   = domain;
    socket->type     = type;
    socket->protocol = protocol;
    socket->ops      = &socket_unix_ops;
    socket->p        = NULL;

    file->flags     |= FILE_SOCKET;
    file->socket     = socket;
    return 0;
}

/**
 * Makes two `AF_UNIX` sockets connected to each other, for
 * `socketpair()`: a connection as `accept()` makes one, `a` its server
 * side and `b` its client side.
 *
 * @param a         the open file that becomes one socket
 * @param b         the open file that becomes the other
 * @param type      `SOCK_STREAM` (the only one supported)
 * @param protocol  0
 * @return 0, `-EPROTONOSUPPORT`, or `-ENOMEM`
 */
int socket_unix_pair(struct file *a, struct file *b, int type, int protocol)
{
    if (type != SOCK_STREAM || protocol)
        return -EPROTONOSUPPORT;

    struct socket *sa = kmalloc(sizeof(struct socket), &M_SOCKET, M_ZERO);
    struct socket *sb = kmalloc(sizeof(struct socket), &M_SOCKET, M_ZERO);
    struct un_conn *conn = kmalloc(sizeof(struct un_conn), &M_UN_CONN, M_ZERO);
    struct ringbuf *soci = ringbuf_new(SOCKBUF);
    struct ringbuf *sico = ringbuf_new(SOCKBUF);

    if (!sa || !sb || !conn || !soci || !sico) {
        kfree(sa);
        kfree(sb);
        kfree(conn);
        if (soci) ringbuf_free(soci);
        if (sico) ringbuf_free(sico);
        return -ENOMEM;
    }

    conn->soci      = soci;
    conn->sico      = sico;
    conn->connected = 1;
    conn->sides     = 2;

    *sa = (struct socket) {
        .domain   = AF_UNIX_CONN,
        .type     = type,
        .protocol = protocol,
        .ops      = &socket_unix_ops,
        .p        = conn,
    };
    *sb = *sa;

    /* offset 0 marks the server side, 1 the client side */
    a->flags  |= FILE_SOCKET;
    a->socket  = sa;
    a->offset  = 0;

    b->flags  |= FILE_SOCKET;
    b->socket  = sb;
    b->offset  = 1;

    return 0;
}

/** Accepts a connection request on a listening socket, waiting for one
 * unless the socket doesn't block. `conn` becomes the server's side of the
 * connection. The peer's address isn't given: names aren't kept. */
static int socket_unix_accept(struct file *file, struct file *conn, const struct sockaddr *addr, socklen_t *len)
{
    if (!file || !file->socket)
        return -EINVAL;

    struct un_socket *socket = file->socket->p;

    if (file->socket->domain != AF_UNIX || !socket || !socket->listening)
        return -EINVAL;

    /* sleep until a connection request comes */
    while (!socket->requests.count) {
        if (file->flags & O_NONBLOCK)
            return -EAGAIN;

        int err = waitq_sleep(&socket->accept);

        if (err)
            return err;
    }

    /* allocate everything before taking the request off the queue, so
     * that running out of memory leaves it there */
    struct socket *conn_socket = kmalloc(sizeof(struct socket), &M_SOCKET, 0);
    struct ringbuf *soci = ringbuf_new(SOCKBUF);
    struct ringbuf *sico = ringbuf_new(SOCKBUF);

    if (!conn_socket || !soci || !sico) {
        kfree(conn_socket);
        if (soci) ringbuf_free(soci);
        if (sico) ringbuf_free(sico);
        return -ENOMEM;
    }

    struct un_conn *request = dequeue(&socket->requests);

    memcpy(conn_socket, file->socket, sizeof(struct socket));

    conn_socket->domain = AF_UNIX_CONN;
    conn_socket->p      = request;

    /* the connection's server side: offset 0 marks it */
    conn->socket = conn_socket;
    conn->flags  = FILE_SOCKET;
    conn->offset = 0;

    /* the rings, one each way */
    request->soci = soci;
    request->sico = sico;

    request->connected = 1;
    request->sides     = 2;

    /* wake up the client, which waits in connect() */
    waitq_wakeup(&request->connect);

    return 0;
}

/** Binds a socket to a path: a socket node is made there, which must not
 * exist yet (`-EADDRINUSE`). */
static int socket_unix_bind(struct file *file, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_un *addr_un = (struct sockaddr_un *) addr;

    int err = 0;
    struct uio uio = PROC_UIO(curproc);

    if ((err = unix_addr_check(addr)))
        return err;

    /* it is bound or connected already */
    if (file->socket->domain != AF_UNIX || file->socket->p)
        return -EINVAL;

    struct vnode *vnode = NULL;
    err = vfs_lookup(addr_un->sun_path, &uio, &vnode);

    if (err && err != -ENOENT)
        return err;

    if (!err) {
        if (vnode)
            vfs_close(vnode);

        /* the path is taken: POSIX says bind() fails then */
        return -EADDRINUSE;
    }

    /* make the socket node, as Linux does: 0777 less the umask */
    if ((err = vfs_mknod(addr_un->sun_path, S_IFSOCK | 0777, 0, &uio, NULL)))
        return err;

    /* look up the node just made, for a reference to it */
    if ((err = vfs_lookup(addr_un->sun_path, &uio, &vnode)))
        goto unlink;

    struct un_socket *socket = kmalloc(sizeof(struct un_socket), &M_UN_SOCKET, M_ZERO);

    if (!socket) {
        err = -ENOMEM;
        goto close;
    }

    socket->vnode = vnode;

    /* connect() finds a bound socket on this list */
    if (!enqueue(sockets, socket)) {
        kfree(socket);
        err = -ENOMEM;
        goto close;
    }

    file->socket->p = socket;

    return 0;

    /* bind() failed after making the node: the node goes, so that the path
     * can be bound again */
close:
    vfs_close(vnode);
unlink:
    vfs_unlink(addr_un->sun_path, &uio);
    return err;
}

/** Connects to the socket bound to a path: the request waits on the
 * listening socket until it is accepted, or refused because the listener
 * closed (`-ECONNREFUSED`). */
static int socket_unix_connect(struct file *file, const struct sockaddr *addr, socklen_t len)
{
    struct sockaddr_un *addr_un = (struct sockaddr_un *) addr;

    int err = 0;
    struct vnode *vnode = NULL;
    struct uio uio = PROC_UIO(curproc);

    if ((err = unix_addr_check(addr)))
        return err;

    if (file->socket->domain != AF_UNIX)
        return -EISCONN;

    /* find the socket bound to the path */
    if ((err = vfs_lookup(addr_un->sun_path, &uio, &vnode))) {
        return err;
    }

    if (!vnode)
        return -ENOENT;

    struct un_socket *socket = NULL;

    queue_for (node, sockets) {
        struct un_socket *_socket = (struct un_socket *) node->value;
        if (_socket->vnode == vnode) {
            socket = _socket;
            break;
        }
    }

    /* the bound socket holds its own reference to the vnode */
    vfs_close(vnode);

    /* the path isn't a bound socket, or it isn't listening, or its backlog
     * is full */
    if (!socket || !socket->listening || socket->requests.count >= socket->backlog)
        return -ECONNREFUSED;

    struct un_conn *request = kmalloc(sizeof(struct un_conn), &M_UN_CONN, M_ZERO);
    if (!request) return -ENOMEM;

    if (!enqueue(&socket->requests, request)) {
        kfree(request);
        return -ENOMEM;
    }

    /* wake up the server, if it waits in accept() */
    waitq_wakeup(&socket->accept);

    /* sleep until the connection is accepted, or refused */
    while (!request->connected) {
        int err = waitq_sleep(&request->connect);

        /* a signal gives the connection up, unless it was accepted or
         * refused meanwhile. The request is still on the listener's queue
         * then (the listener is alive: closing it would have refused the
         * request), and is taken off it, so accept() never sees it */
        if (err && !request->connected) {
            queue_remove(&socket->requests, request);
            kfree(request);
            return err;
        }
    }

    /* the listening socket was closed before accepting it */
    if (request->connected < 0) {
        kfree(request);
        return -ECONNREFUSED;
    }

    /* the connection's client side: offset 1 marks it */
    file->socket->domain = AF_UNIX_CONN;
    file->socket->p = request;
    file->offset    = 1;

    return 0;
}

/** Makes a bound socket listen for connections. */
static int socket_unix_listen(struct file *file, int backlog)
{
    struct un_socket *socket = (struct un_socket *) file->socket->p;

    /* it must be bound (and not a connection) */
    if (file->socket->domain != AF_UNIX || !socket)
        return -EINVAL;

    /* allow at least one pending connection */
    socket->backlog = MIN(MAX(backlog, 1), SOMAXCONN);
    socket->listening = 1;

    return 0;
}

/**
 * Moves up to `n` bytes from a connection's ring to user memory, a chunk
 * at a time through the kernel's memory: copying to user memory may sleep
 * (a page brought in from the disk), and the peer may free the ring
 * meanwhile (`*ring_p` is NULL then). Returns how many bytes moved, or
 * `-EFAULT` if a fault came first.
 */
static ssize_t ring_to_iobuf(struct ringbuf **ring_p, size_t n, struct iobuf *io)
{
    char chunk[256];
    size_t done = 0;

    while (done < n && *ring_p) {
        size_t len = ringbuf_read_noconsume(*ring_p, 0, MIN(n - done, sizeof(chunk)), chunk);

        if (!len)
            break;

        ssize_t r = iobuf_out(io, chunk, len);

        if (r < 0)
            return done ? (ssize_t) done : r;

        /* what was copied out comes off the ring */
        if (*ring_p)
            ringbuf_discard(*ring_p, r);

        done += r;

        if ((size_t) r < len)
            break;
    }

    return done;
}

/** Moves up to `n` bytes from user memory to a connection's ring, as
 * `ring_to_iobuf()` does the other way. Returns how many bytes moved, or
 * `-EFAULT` if a fault came first. */
static ssize_t iobuf_to_ring(struct ringbuf **ring_p, size_t n, struct iobuf *io)
{
    char chunk[256];
    size_t done = 0;

    while (done < n && *ring_p) {
        ssize_t len = iobuf_peek(io, chunk, MIN(n - done, sizeof(chunk)));

        if (len <= 0)
            return done ? (ssize_t) done : len;

        if (!*ring_p)
            break;

        size_t w = ringbuf_write(*ring_p, len, chunk);
        iobuf_skip(io, w);
        done += w;

        if (w < (size_t) len || io->fault)
            break;
    }

    return done;
}

/** Sends data on a connection: as much as the ring takes, waiting for
 * room unless the socket doesn't block (with `MSG_WAITALL`, until all is
 * sent). Returns how much was sent, or a negative errno (`-EPIPE` once
 * writing is shut down, or once the peer can't read anymore). */
static ssize_t socket_unix_send(struct file *file, struct iobuf *io, int flags,
                                const struct sockaddr *to, socklen_t tolen)
{
    size_t len = io->resid;

    if (file->socket->domain != AF_UNIX_CONN)
        return -ENOTCONN;

    struct un_conn *conn = file->socket->p;

    /* the peer may free the ring by shutting down while we sleep, so keep
     * a pointer to the connection's field and read it again each time */
    struct ringbuf **ring_p = NULL;

    struct waitq *recv_queue = NULL;
    struct waitq *send_queue = NULL;

    if (file->offset == 0) {  /* Server */
        ring_p     = &conn->soci;
        recv_queue = &conn->client_recv;
        send_queue = &conn->server_send;
    } else {    /* Client */
        ring_p     = &conn->sico;
        recv_queue = &conn->server_recv;
        send_queue = &conn->client_send;
    }

    /* writing is shut down */
    if (file->offset == 0 ? conn->soci_end : conn->sico_end)
        return -EPIPE;

    ssize_t retval = 0;
    ssize_t incr   = 0;

    for (;;) {
        struct ringbuf *ring = *ring_p;

        /* the peer is gone, or shut for reading: what was sent, or
         * EPIPE if nothing was (a write of 0 would be made again and
         * again) */
        if (!ring)
            return retval ? retval : -EPIPE;

        size_t wlen = MIN(len - retval, SOCKBUF - ringbuf_available(ring));

        if ((incr = iobuf_to_ring(ring_p, wlen, io)) < 0)
            return retval ? retval : incr;

        if (incr > 0)
            waitq_wakeup(recv_queue);

        if (incr > 0 && !(flags & MSG_WAITALL))
            return incr;

        retval += incr;

        if ((size_t) retval == len || io->fault)
            return retval;

        /* the ring is full: wait, unless the socket doesn't block */
        if (file->flags & O_NONBLOCK)
            return retval ? retval : -EAGAIN;

        int err = waitq_sleep(send_queue);

        if (err)
            return retval ? retval : err;
    }
}

/** Receives data from a connection: what the ring has, waiting for data
 * unless the socket doesn't block (with `MSG_WAITALL`, until the buffer
 * is full). Returns how much was received, 0 at the end, or a negative
 * errno. */
static ssize_t socket_unix_recv(struct file *file, struct iobuf *io, int flags,
                                struct sockaddr *from, socklen_t *fromlen)
{
    size_t len = io->resid;

    if (file->socket->domain != AF_UNIX_CONN)
        return -ENOTCONN;

    /* the peer is unnamed (names aren't kept) */
    if (from && fromlen) {
        if (*fromlen >= sizeof(sa_family_t))
            from->sa_family = AF_UNIX;
        *fromlen = sizeof(sa_family_t);
    }

    struct un_conn *conn = file->socket->p;

    /* see socket_unix_send() */
    struct ringbuf **ring_p = NULL;

    struct waitq *recv_queue = NULL;
    struct waitq *send_queue = NULL;

    int *end_p;

    if (file->offset == 0) {  /* Server */
        ring_p     = &conn->sico;
        end_p      = &conn->sico_end;
        recv_queue = &conn->server_recv;
        send_queue = &conn->client_send;
    } else {    /* Client */
        ring_p     = &conn->soci;
        end_p      = &conn->soci_end;
        recv_queue = &conn->client_recv;
        send_queue = &conn->server_send;
    }

    ssize_t retval = 0;
    ssize_t incr   = 0;

    for (;;) {
        struct ringbuf *ring = *ring_p;

        if (!ring)  /* the connection was closed */
            return retval;

        if ((incr = ring_to_iobuf(ring_p, len - retval, io)) < 0)
            return retval ? retval : incr;

        if (incr > 0)
            waitq_wakeup(send_queue);

        /* the peer is done writing: nothing more will come */
        if (!incr && *end_p)
            return retval;

        if (incr > 0 && !(flags & MSG_WAITALL))
            return incr;

        retval += incr;

        if ((size_t) retval == len || io->fault)
            return retval;

        /* the ring is empty: wait, unless the socket doesn't block */
        if (file->flags & O_NONBLOCK)
            return retval ? retval : -EAGAIN;

        int err = waitq_sleep(recv_queue);

        if (err)
            return retval ? retval : err;
    }
}

/** Says whether a read (or an accept) wouldn't wait: how many bytes or
 * connection requests wait, or 1 at the end of the data. */
static int socket_unix_can_read(struct file *file, size_t len)
{
    if (!file || !file->socket)
        return -EINVAL;

    if (file->socket->domain == AF_UNIX) {
        struct un_socket *socket = file->socket->p;
        return socket? (int) socket->requests.count : 0;
    }

    if (file->socket->domain != AF_UNIX_CONN)
        return -ENOTCONN;

    struct un_conn *conn = file->socket->p;

    struct ringbuf *ring = NULL;

    int end;

    if (file->offset == 0) {
        /* Server */
        ring = conn->sico;
        end  = conn->sico_end;
    } else {
        /* Client */
        ring = conn->soci;
        end  = conn->soci_end;
    }

    if (!ring)
        return -ENOTCONN;   /* reading was shut down */

    /* at the end, a read doesn't wait either: it returns 0 */
    size_t available = ringbuf_available(ring);
    return available ? (int) available : end;
}

/** Returns the wait queue that a socket's readers (or writers) sleep on: a
 * listener's `accept`, or the queues of a connection's side. */
static struct waitq *socket_unix_wait_queue(struct file *file, int write)
{
    if (file->socket->domain == AF_UNIX) {
        struct un_socket *socket = file->socket->p;
        return socket && !write ? &socket->accept : NULL;
    }

    if (file->socket->domain != AF_UNIX_CONN)
        return NULL;

    struct un_conn *conn = file->socket->p;

    if (file->offset == 0)  /* Server */
        return write ? &conn->server_send : &conn->server_recv;
    else                    /* Client */
        return write ? &conn->client_send : &conn->client_recv;
}

/** Says how much a write could send without waiting: the room in the
 * ring. */
static int socket_unix_can_write(struct file *file, size_t len)
{
    if (file->socket->domain != AF_UNIX_CONN)
        return -ENOTCONN;

    struct un_conn *conn = file->socket->p;

    struct ringbuf *ring = NULL;

    if (file->offset == 0) {
        /* Server */
        ring = conn->soci;
    } else {
        /* Client */
        ring = conn->sico;
    }

    if (!ring)
        return -ENOTCONN;

    return ring->size - ringbuf_available(ring);
}


/** Shuts down one side of a connection for reading, writing or both. Shut
 * for reading, the ring it reads is freed, so the peer can't write
 * anymore. Shut for writing, the ring it writes ends: the peer reads what
 * is left, then the end. */
static void unix_conn_shut(struct file *file, int how)
{
    struct un_conn *conn = file->socket->p;
    int client = file->offset;

    struct ringbuf **in = client ? &conn->soci : &conn->sico;
    int *out_end = client ? &conn->sico_end : &conn->soci_end;

    if ((how & SHUT_RD) && *in) {
        ringbuf_free(*in);
        *in = NULL;
    }

    if (how & SHUT_WR)
        *out_end = 1;

    /* wake up the peer, which may wait on a ring that went or ended */
    unix_conn_wakeup(conn);
}

/** Handles `shutdown()`, which only applies to a connection. */
static int socket_unix_shut(struct file *file, int how)
{
    if (!file || !file->socket)
        return -EINVAL;

    if (file->socket->domain != AF_UNIX_CONN)
        return -ENOTCONN;

    unix_conn_shut(file, how);
    return 0;
}

/** Frees a socket when its last descriptor is closed. A connection's side
 * is shut down, and the connection is freed once both sides are gone. A
 * listener refuses the requests that wait, and is unbound. */
static int socket_unix_shutdown(struct file *file, int how)
{
    if (!file || !file->socket)
        return -EINVAL;
    
    if (file->socket->domain == AF_UNIX_CONN) {
        struct un_conn *conn = file->socket->p;

        unix_conn_shut(file, how);

        /* this side is gone; the connection goes once both are. Both
         * rings are gone then: each side freed the one it read. (The
         * rings alone don't say so: a side shut for reading has freed
         * its ring, and is still open.) */
        if (!--conn->sides)
            kfree(conn);

        kfree(file->socket);
        file->socket = NULL;

        return 0;
    }

    /* not a connection: unbind it, if it is bound */
    struct un_socket *socket = file->socket->p;

    if (socket) {
        struct un_conn *request;

        queue_remove(sockets, socket);

        /* refuse the waiting connection requests; connect() frees each */
        while ((request = dequeue(&socket->requests))) {
            request->connected = -1;
            waitq_wakeup(&request->connect);
        }

        vfs_close(socket->vnode);
        kfree(socket);
    }

    kfree(file->socket);
    file->socket = NULL;

    return 0;
}

/** Gets a socket's name or its peer's: always unnamed, since names aren't
 * kept. */
static int socket_unix_getname(struct file *file, struct sockaddr *addr, socklen_t *len, int peer)
{
    if (*len >= sizeof(sa_family_t))
        addr->sa_family = AF_UNIX;

    *len = sizeof(sa_family_t);
    return 0;
}

/** Handles `FIONREAD`: how many bytes a connection has to read. */
static int socket_unix_ioctl(struct file *file, int request, void *argp)
{
    if (request == FIONREAD) {
        int n = socket_unix_can_read(file, 0);

        /* a listening socket has connection requests, not bytes, and the
         * end of a connection is 0 bytes */
        n = file->socket->domain == AF_UNIX_CONN && n > 0 ? n : 0;
        return copyout(argp, &n, sizeof(n));
    }

    return -ENOTTY;
}

static struct sock_ops socket_unix_ops = {
    .accept    = socket_unix_accept,
    .bind      = socket_unix_bind,
    .connect   = socket_unix_connect,
    .listen    = socket_unix_listen,
    .recv      = socket_unix_recv,
    .send      = socket_unix_send,
    .can_read  = socket_unix_can_read,
    .can_write = socket_unix_can_write,
    .wait_queue = socket_unix_wait_queue,
    .shutdown  = socket_unix_shutdown,
    .shut      = socket_unix_shut,
    .getname   = socket_unix_getname,
    .ioctl     = socket_unix_ioctl,
};
