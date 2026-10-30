#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>

/* address families, socket types and struct sockaddr, shared with the kernel */
#include <aquila/socket.h>

/* any address */
struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_data[124];
} __attribute__((aligned(8)));

/* socket()'s, socketpair()'s and accept4()'s flags, or-ed with the type
 * (the C library sets them on the descriptor, as fcntl() does, but for
 * socketpair(), whose system call takes them) */
#define SOCK_NONBLOCK   0x4000      /* O_NONBLOCK */
#define SOCK_CLOEXEC    0x40000     /* O_CLOEXEC */

/* sendmsg() and recvmsg(): data from and to several buffers; ancillary
 * data (descriptors, SCM_RIGHTS) isn't passed: EOPNOTSUPP */
struct iovec;

struct msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    int           msg_iovlen;
    void         *msg_control;
    socklen_t     msg_controllen;
    int           msg_flags;
};

struct cmsghdr {
    socklen_t cmsg_len;
    int       cmsg_level;
    int       cmsg_type;
};

#define SCM_RIGHTS      1

#define CMSG_ALIGN(len)     (((len) + sizeof(long) - 1) & ~(sizeof(long) - 1))
#define CMSG_DATA(cmsg)     ((unsigned char *) (cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_SPACE(len)     (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(len))
#define CMSG_LEN(len)       (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_FIRSTHDR(mhdr) \
    ((mhdr)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr *) (mhdr)->msg_control : (struct cmsghdr *) 0)
#define CMSG_NXTHDR(mhdr, cmsg) \
    ((unsigned char *) (cmsg) + CMSG_ALIGN((cmsg)->cmsg_len) + sizeof(struct cmsghdr) > \
     (unsigned char *) (mhdr)->msg_control + (mhdr)->msg_controllen ? (struct cmsghdr *) 0 : \
     (struct cmsghdr *) ((unsigned char *) (cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))

struct linger {
    int l_onoff;
    int l_linger;
};

int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fd[2]);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);

ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen);

int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int getsockopt(int fd, int level, int name, void *val, socklen_t *len);
int setsockopt(int fd, int level, int name, const void *val, socklen_t len);

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags);
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags);
ssize_t recvmsg(int fd, struct msghdr *msg, int flags);

#endif /* ! _SYS_SOCKET_H */
