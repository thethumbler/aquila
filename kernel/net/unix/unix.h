#ifndef _NET_UNIX_H
#define _NET_UNIX_H

/*
 * Unix domain sockets' private structures (net/unix/unix.c).
 */

#define AF_UNIX_CONN    255

/** A connection between a client socket and the server that accepted it,
 * or between the two sockets of a `socketpair()`. */
struct un_conn {
    struct ringbuf *soci;   /* data the server sends and the client receives */
    struct ringbuf *sico;   /* data the client sends and the server receives */

    /* Set when the writer of a ring is done (shutdown or close): the reader
     * reads what is left, then sees the end, and frees the ring. */
    int soci_end, sico_end;

    struct waitq server_recv;
    struct waitq server_send;
    struct waitq client_recv;
    struct waitq client_send;
    struct waitq connect;

    /* 1 once accepted, -1 if refused because the listener closed */
    int connected;

    /* how many sides are open: the connection goes with the last one
     * (each side frees the ring it reads, when it's shut for reading) */
    int sides;
};

/** A listening socket, bound to a name in the file system. */
struct un_socket {
    struct vnode *vnode;

    size_t backlog;    /* how many connection requests may wait */

    struct queue requests;
    struct waitq accept;

    int listening;
};

#include <aquila/un.h>

#endif /* ! _NET_UNIX_H */
