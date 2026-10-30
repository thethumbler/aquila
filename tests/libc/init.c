/*
 * C library on the kernel
 *
 * Built with i686-aquila-gcc against newlib (unlike the other tests),
 * checks that what programs see through libc matches the kernel: errno
 * values, signal numbers, struct stat, and a few library functions that
 * depend on the port (stdio, getline, opendir, pipes, fork/wait, sockets,
 * vectored I/O, the interval timer, addresses, pseudo-terminals, random
 * bytes, TCP over loopback).
 *
 * Reports in the aqtest format on the serial port.
 */

/* pipe2() and dup3() */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <crypt.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <pwd.h>
#include <sched.h>
#include <shadow.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <wchar.h>

static FILE *out;
static int fails;

static void check(const char *name, long got, long want)
{
    fprintf(out, "%s %s got=%ld want=%ld\n", got == want ? "PASS" : "FAIL", name, got, want);
    if (got != want)
        ++fails;
}

static volatile sig_atomic_t got_signal;
static sigjmp_buf fault_jmp;
static void *volatile fault_addr;
static volatile int fault_code;

/* SA_SIGINFO: the faulting address and why, then back to the test */
static void fault_handler(int sig, siginfo_t *info, void *ctx)
{
    fault_addr = info->si_addr;
    fault_code = info->si_code;
    siglongjmp(fault_jmp, sig);
}

static void handler(int sig)
{
    got_signal = sig;
}

/* virtual terminal 2 (VGA text mode, 80x25 under QEMU -kernel) */
static volatile int alarms;
static void alarm_handler(int sig) { alarms++; (void) sig; }

/* what the libc port adds for X: vectored I/O, addresses, socket calls,
 * the interval timer */
static void test_x_support(void)
{
    /* writev: one write, readv: one read spread over the vectors */
    int p[2];
    pipe(p);
    struct iovec wv[3] = {{"ab", 2}, {"", 0}, {"cde", 3}};
    check("writev", writev(p[1], wv, 3), 5);
    char r1[2], r2[8] = {0};
    struct iovec rv[2] = {{r1, 2}, {r2, sizeof(r2)}};
    check("readv", readv(p[0], rv, 2), 5);
    check("readv spread", r1[0] == 'a' && r1[1] == 'b' && !strcmp(r2, "cde"), 1);
    errno = 0;
    check("writev bad count -> EINVAL", writev(p[1], wv, -1) == -1 ? errno : 0, EINVAL);
    close(p[0]);
    close(p[1]);

    /* addresses */
    struct in_addr in;
    char text[INET_ADDRSTRLEN];
    check("inet_pton", inet_pton(AF_INET, "192.168.1.20", &in), 1);
    check("inet_pton order", in.s_addr, htonl(0xc0a80114));
    check("inet_pton bad", inet_pton(AF_INET, "192.168.1", &in), 0);
    check("inet_pton leading zero", inet_pton(AF_INET, "1.02.3.4", &in), 0);
    check("inet_ntop", strcmp(inet_ntop(AF_INET, &in.s_addr, text, sizeof(text)) ? text : "", "1.02.3.4") != 0, 1);
    in.s_addr = htonl(0x7f000001);
    check("inet_ntoa", strcmp(inet_ntoa(in), "127.0.0.1"), 0);
    check("inet_aton short form", inet_aton("10.1", &in) && in.s_addr == htonl(0x0a000001), 1);
    check("inet_addr", inet_addr("1.2.3.4"), htonl(0x01020304));
    check("inet_addr bad", inet_addr("1.2.3.256"), INADDR_NONE);

    /* names: only numeric hosts (no resolver) */
    struct addrinfo *res = NULL, hints = {0};
    hints.ai_socktype = SOCK_STREAM;
    check("getaddrinfo numeric", getaddrinfo("10.0.0.1", "6000", &hints, &res), 0);
    struct sockaddr_in *sin = res ? (struct sockaddr_in *) res->ai_addr : NULL;
    check("getaddrinfo address", sin && sin->sin_addr.s_addr == htonl(0x0a000001) && sin->sin_port == htons(6000), 1);
    freeaddrinfo(res);
    /* no DNS server answers yet (test_resolver() has one): try again */
    check("getaddrinfo name, no server", getaddrinfo("example.org", NULL, NULL, &res), EAI_AGAIN);
    check("getaddrinfo bad service", getaddrinfo("1.2.3.4", "no-such-service", NULL, &res), EAI_SERVICE);
    check("getaddrinfo x11 (/etc/services)", getaddrinfo("1.2.3.4", "x11", NULL, &res), 0);
    freeaddrinfo(res);
    struct hostent *he = gethostbyname("10.0.0.2");
    check("gethostbyname numeric", he && he->h_length == 4 && !strcmp(he->h_name, "10.0.0.2"), 1);
    he = gethostbyname("localhost");
    check("gethostbyname localhost", he && ((struct in_addr *) he->h_addr_list[0])->s_addr == htonl(0x7f000001), 1);

    /* socket calls on a Unix socket */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    check("getsockname", getsockname(s, (struct sockaddr *) &ss, &len), 0);
    check("getsockname family", ss.ss_family, AF_UNIX);
    int val = -1;
    len = sizeof(val);
    check("getsockopt SO_TYPE", getsockopt(s, SOL_SOCKET, SO_TYPE, &val, &len) == 0 && val == SOCK_STREAM, 1);
    check("getsockopt SO_ERROR", getsockopt(s, SOL_SOCKET, SO_ERROR, &val, &len) == 0 && val == 0, 1);
    val = 65536;
    check("setsockopt SO_SNDBUF", setsockopt(s, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val)), 0);
    errno = 0;
    check("setsockopt other level -> ENOPROTOOPT", setsockopt(s, 6, 1, &val, sizeof(val)) == -1 ? errno : 0, ENOPROTOOPT);
    errno = 0;
    len = sizeof(ss);
    int nf = open("/", O_RDONLY);
    check("getpeername not a socket -> ENOTSOCK", getpeername(nf, (struct sockaddr *) &ss, &len) == -1 ? errno : 0, ENOTSOCK);
    close(nf);
    errno = 0;
    check("shutdown unconnected -> ENOTCONN", shutdown(s, SHUT_RDWR) == -1 ? errno : 0, ENOTCONN);
    close(s);

    /* no System V shared memory: programs check and go without */
    errno = 0;
    check("shmget -> ENOSYS", shmget(IPC_PRIVATE, 4096, IPC_CREAT) == -1 ? errno : 0, ENOSYS);
    check("getpagesize", getpagesize(), 4096);
    check("getdtablesize", getdtablesize() > 20, 1);

    /* gettimeofday counts below the second */
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    usleep(120000);
    gettimeofday(&t1, NULL);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
    check("gettimeofday resolution", ms >= 110 && ms < 900, 1);

    /* the interval timer, alarm() and pause() */
    signal(SIGALRM, alarm_handler);
    struct itimerval it = {{0, 30000}, {0, 30000}}, old;
    check("setitimer", setitimer(ITIMER_REAL, &it, NULL), 0);
    while (alarms < 3)
        pause();
    check("SIGALRM by setitimer", alarms, 3);
    check("getitimer", getitimer(ITIMER_REAL, &old) == 0 && old.it_interval.tv_usec == 30000, 1);
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL);
    errno = 0;
    check("setitimer virtual -> EINVAL", setitimer(ITIMER_VIRTUAL, &it, NULL) == -1 ? errno : 0, EINVAL);
    check("alarm none pending", alarm(5), 0);
    check("alarm pending", alarm(0), 5);
    alarms = 0;
    alarm(1);
    pause();
    check("alarm fired", alarms, 1);
    signal(SIGALRM, SIG_DFL);
}

/* pthreads: a counter two threads add to under a mutex, a condition one
 * waits on, a key with a value each */
static pthread_mutex_t pt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pt_cond = PTHREAD_COND_INITIALIZER;
static pthread_key_t pt_key;
static volatile int pt_count, pt_ready, pt_once_runs;
static volatile pthread_t pt_self;
/* TCP over loopback, as programs use it */
static void test_tcp(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    char buf[32];

    check("sizeof(struct sockaddr) holds a sockaddr_in", sizeof(struct sockaddr) >= sizeof(addr), 1);

    int l = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    check("tcp bind", bind(l, (struct sockaddr *) &addr, sizeof(addr)), 0);
    check("tcp listen", listen(l, 4), 0);
    check("tcp getsockname", getsockname(l, (struct sockaddr *) &addr, &len), 0);
    check("tcp: an ephemeral port", ntohs(addr.sin_port) >= 49152, 1);

    int c = socket(AF_INET, SOCK_STREAM, 0);
    check("tcp connect", connect(c, (struct sockaddr *) &addr, sizeof(addr)), 0);
    check("tcp TCP_NODELAY", setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)), 0);

    struct sockaddr_in peer;
    len = sizeof(peer);
    int s = accept(l, (struct sockaddr *) &peer, &len);
    check("tcp accept", s >= 0, 1);
    check("tcp accept: from 127.0.0.1", peer.sin_addr.s_addr == inet_addr("127.0.0.1"), 1);

    FILE *f = fdopen(c, "w");
    fprintf(f, "GET / HTTP/1.0\r\n\r\n");
    fflush(f);
    check("tcp recv", recv(s, buf, sizeof(buf), 0), 18);
    check("tcp: through stdio", memcmp(buf, "GET / HTTP/1.0\r\n\r\n", 18), 0);

    check("tcp send", send(s, "HTTP/1.0 200 OK", 15, 0), 15);
    shutdown(s, SHUT_WR);
    int n = read(c, buf, sizeof(buf));
    check("tcp read", n, 15);
    check("tcp: then EOF", read(c, buf, sizeof(buf)), 0);

    fclose(f);
    close(s);

    /* SOCK_NONBLOCK, SOCK_CLOEXEC and accept4(), set by the C library */
    c = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    check("SOCK_NONBLOCK", (fcntl(c, F_GETFL) & O_NONBLOCK) != 0, 1);
    check("SOCK_CLOEXEC", fcntl(c, F_GETFD) & FD_CLOEXEC, FD_CLOEXEC);
    check("tcp connect non-blocking", connect(c, (struct sockaddr *) &addr, sizeof(addr)) == -1 ? errno : 0, EINPROGRESS);

    struct pollfd pfd = {c, POLLOUT, 0};
    poll(&pfd, 1, 2000);
    s = accept4(l, NULL, NULL, SOCK_NONBLOCK);
    check("accept4", s >= 0 && (fcntl(s, F_GETFL) & O_NONBLOCK), 1);
    check("accept4 bad flags", accept4(l, NULL, NULL, 0x1) == -1 ? errno : 0, EINVAL);

    /* sendmsg() and recvmsg(): the pieces, no control data */
    struct iovec out[2] = {{"msg-", 4}, {"pieces", 6}};
    struct msghdr mh = {0};
    mh.msg_iov = out;
    mh.msg_iovlen = 2;
    check("sendmsg", sendmsg(c, &mh, 0), 10);

    char a[3], b[16] = {0};
    struct iovec in[2] = {{a, 3}, {b, sizeof(b) - 1}};
    struct msghdr rh = {0};
    rh.msg_iov = in;
    rh.msg_iovlen = 2;
    pfd = (struct pollfd) {s, POLLIN, 0};
    poll(&pfd, 1, 2000);
    check("recvmsg", recvmsg(s, &rh, 0), 10);
    check("recvmsg: the pieces", !memcmp(a, "msg", 3) && !strcmp(b, "-pieces"), 1);

    char cbuf[CMSG_SPACE(sizeof(int))];
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    check("sendmsg with control data -> EOPNOTSUPP", sendmsg(c, &mh, 0) == -1 ? errno : 0, EOPNOTSUPP);

    close(c);
    close(s);
    close(l);
}

/*
 * The resolver, against a DNS server of ours: a child on 127.0.0.1:53,
 * UDP and TCP (/etc/resolv.conf says so, rootfs/)
 */

static int dns_put_name(unsigned char *p, const char *name)
{
    int n = 0;

    while (*name) {
        const char *dot = strchr(name, '.');
        int len = dot ? dot - name : (int) strlen(name);
        p[n++] = len;
        memcpy(p + n, name, len);
        n += len;
        name += len + (dot != NULL);
    }

    p[n++] = 0;
    return n;
}

/* an answer for the question in q (qlen bytes); its length */
static int dns_answer(const unsigned char *q, int qlen, unsigned char *a, int tcp)
{
    char name[256];
    int off = 12, o = 0;

    while (q[off] && off < qlen) {
        if (o)
            name[o++] = '.';
        memcpy(name + o, q + off + 1, q[off]);
        o += q[off];
        off += q[off] + 1;
    }

    name[o] = '\0';
    int qtype = q[off + 2];
    int qend = off + 5;

    memcpy(a, q, qend);
    a[2] = 0x81;            /* a response, recursion desired */
    a[3] = 0x80;            /* recursion available */
    a[6] = a[7] = 0;

    int n = qend, an = 0;

    /* an answer for the question's name (a pointer to it, offset 12) */
#define RR(type, len) do { \
        a[n++] = 0xc0; a[n++] = 12; a[n++] = 0; a[n++] = (type); a[n++] = 0; a[n++] = 1; \
        a[n++] = 0; a[n++] = 0; a[n++] = 0; a[n++] = 60; a[n++] = 0; a[n++] = (len); an++; \
    } while (0)
#define A(x, y, z, w) do { RR(1, 4); a[n++] = x; a[n++] = y; a[n++] = z; a[n++] = w; } while (0)

    if (qtype == 1 && !strcmp(name, "example.test")) {
        A(192, 0, 2, 7);
        A(192, 0, 2, 8);
    } else if (qtype == 1 && !strcmp(name, "alias.test")) {
        unsigned char target[64];
        int tlen = dns_put_name(target, "example.test");
        RR(5, tlen);
        memcpy(a + n, target, tlen);
        n += tlen;
        /* example.test's A, its name in full */
        n += dns_put_name(a + n, "example.test");
        a[n++] = 0; a[n++] = 1; a[n++] = 0; a[n++] = 1;
        a[n++] = 0; a[n++] = 0; a[n++] = 0; a[n++] = 60; a[n++] = 0; a[n++] = 4;
        a[n++] = 192; a[n++] = 0; a[n++] = 2; a[n++] = 7;
        an++;
    } else if (qtype == 1 && !strcmp(name, "short.test")) {
        A(192, 0, 2, 9);
    } else if (qtype == 1 && !strcmp(name, "big.test")) {
        /* too big for UDP, says the server: over TCP */
        if (!tcp)
            a[2] |= 0x02;
        else
            A(192, 0, 2, 10);
    } else if (qtype == 12 && !strcmp(name, "7.2.0.192.in-addr.arpa")) {
        unsigned char target[64];
        int tlen = dns_put_name(target, "example.test");
        RR(12, tlen);
        memcpy(a + n, target, tlen);
        n += tlen;
    } else {
        a[3] |= 3;          /* no such name */
    }

#undef A
#undef RR

    a[7] = an;
    return n;
}

static void dns_server(void)
{
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_port = htons(53);
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int u = socket(AF_INET, SOCK_DGRAM, 0), t = socket(AF_INET, SOCK_STREAM, 0);

    if (bind(u, (struct sockaddr *) &me, sizeof(me)) || bind(t, (struct sockaddr *) &me, sizeof(me)) ||
        listen(t, 4))
        _exit(1);

    /* ready */
    write(3, "r", 1);

    unsigned char q[512], a[1024];

    for (;;) {
        struct pollfd pfd[2] = {{u, POLLIN, 0}, {t, POLLIN, 0}};
        poll(pfd, 2, -1);

        if (pfd[0].revents) {
            struct sockaddr_in from;
            socklen_t len = sizeof(from);
            int n = recvfrom(u, q, sizeof(q), 0, (struct sockaddr *) &from, &len);

            if (n >= 12) {
                int r = dns_answer(q, n, a, 0);
                sendto(u, a, r, 0, (struct sockaddr *) &from, len);
            }
        }

        if (pfd[1].revents) {
            int c = accept(t, NULL, NULL);
            unsigned char hdr[2];

            if (c >= 0 && recv(c, hdr, 2, MSG_WAITALL) == 2) {
                int n = hdr[0] << 8 | hdr[1];

                if (n <= (int) sizeof(q) && recv(c, q, n, MSG_WAITALL) == n) {
                    int r = dns_answer(q, n, a, 1);
                    hdr[0] = r >> 8;
                    hdr[1] = r & 0xff;
                    send(c, hdr, 2, 0);
                    send(c, a, r, 0);
                }
            }

            close(c);
        }
    }
}

static int has_addr(struct addrinfo *res, const char *addr)
{
    for (; res; res = res->ai_next)
        if (((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr == inet_addr(addr))
            return 1;

    return 0;
}

static int count_addrs(struct addrinfo *res)
{
    int n = 0;

    for (; res; res = res->ai_next)
        n++;

    return n;
}

static void test_resolver(void)
{
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* /etc/hosts, no server needed */
    check("getaddrinfo localhost", getaddrinfo("localhost", "80", &hints, &res), 0);
    check("getaddrinfo localhost: 127.0.0.1", res && has_addr(res, "127.0.0.1") &&
          ntohs(((struct sockaddr_in *) res->ai_addr)->sin_port) == 80, 1);
    freeaddrinfo(res);

    hints.ai_flags = AI_CANONNAME;
    check("getaddrinfo from /etc/hosts (an alias)", getaddrinfo("filehost", NULL, &hints, &res), 0);
    check("from /etc/hosts: its address and name", res && has_addr(res, "198.51.100.5") &&
          res->ai_canonname && !strcmp(res->ai_canonname, "filehost.test"), 1);
    freeaddrinfo(res);
    hints.ai_flags = 0;

    /* services */
    struct servent *se = getservbyname("http", "tcp");
    check("getservbyname http", se ? ntohs(se->s_port) : -1, 80);
    se = getservbyport(htons(53), "udp");
    check("getservbyport 53/udp", se && !strcmp(se->s_name, "domain"), 1);
    check("getaddrinfo a service's name", getaddrinfo("127.0.0.1", "https", &hints, &res), 0);
    check("getaddrinfo a service's name: its port", res ? ntohs(((struct sockaddr_in *) res->ai_addr)->sin_port) : -1, 443);
    freeaddrinfo(res);
    check("getaddrinfo AI_NUMERICHOST, a name", ({
        struct addrinfo h = hints; h.ai_flags = AI_NUMERICHOST;
        getaddrinfo("localhost", NULL, &h, &res); }), EAI_NONAME);

    /* the DNS: our server */
    int ready[2];
    pipe(ready);
    pid_t pid = fork();

    if (!pid) {
        dup2(ready[1], 3);
        dns_server();
        _exit(0);
    }

    char r;
    read(ready[0], &r, 1);

    check("dns: getaddrinfo", getaddrinfo("example.test", "80", &hints, &res), 0);
    check("dns: both addresses", count_addrs(res) == 2 && has_addr(res, "192.0.2.7") &&
          has_addr(res, "192.0.2.8"), 1);
    freeaddrinfo(res);

    hints.ai_flags = AI_CANONNAME;
    check("dns: a CNAME", getaddrinfo("alias.test", NULL, &hints, &res), 0);
    check("dns: a CNAME: the canonical name, the address", res && has_addr(res, "192.0.2.7") &&
          res->ai_canonname && !strcmp(res->ai_canonname, "example.test"), 1);
    freeaddrinfo(res);
    hints.ai_flags = 0;

    check("dns: the search domain", getaddrinfo("short", NULL, &hints, &res), 0);
    check("dns: the search domain: short.test", res && has_addr(res, "192.0.2.9"), 1);
    freeaddrinfo(res);

    check("dns: truncated, over TCP", getaddrinfo("big.test", NULL, &hints, &res), 0);
    check("dns: over TCP: the address", res && has_addr(res, "192.0.2.10"), 1);
    freeaddrinfo(res);

    check("dns: no such name", getaddrinfo("nx.test", NULL, &hints, &res), EAI_NONAME);

    struct hostent *he = gethostbyname("example.test");
    check("dns: gethostbyname", he && he->h_length == 4 && he->h_addr_list[0] && he->h_addr_list[1] &&
          !he->h_addr_list[2], 1);
    check("dns: gethostbyname nx", gethostbyname("nx.test") == NULL && h_errno == HOST_NOT_FOUND, 1);

    struct sockaddr_in sin = {0};
    char host[NI_MAXHOST], serv[NI_MAXSERV];
    sin.sin_family = AF_INET;
    sin.sin_port = htons(80);
    sin.sin_addr.s_addr = inet_addr("192.0.2.7");
    check("dns: getnameinfo (PTR)", getnameinfo((struct sockaddr *) &sin, sizeof(sin), host, sizeof(host),
                                                 serv, sizeof(serv), NI_NAMEREQD), 0);
    check("dns: getnameinfo: the name, the service", !strcmp(host, "example.test") && !strcmp(serv, "http"), 1);

    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    getnameinfo((struct sockaddr *) &sin, sizeof(sin), host, sizeof(host), NULL, 0, 0);
    check("getnameinfo from /etc/hosts", strcmp(host, "localhost"), 0);
    sin.sin_addr.s_addr = inet_addr("192.0.2.99");
    check("getnameinfo NI_NAMEREQD, no name", getnameinfo((struct sockaddr *) &sin, sizeof(sin), host,
                                                          sizeof(host), NULL, 0, NI_NAMEREQD), EAI_NONAME);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    close(ready[0]);
    close(ready[1]);
}

/* warn() and err(): the program's name, the message, errno's */
static void test_err(void)
{
    int p[2];
    pipe(p);

    pid_t pid = fork();

    if (!pid) {
        dup2(p[1], 2);
        errno = ENOENT;
        warn("file %d", 7);
        warnx("plain");
        errno = EACCES;
        err(3, "fatal");
    }

    close(p[1]);

    char buf[256] = {0};
    int n = 0, r;

    while ((r = read(p[0], buf + n, sizeof(buf) - 1 - n)) > 0)
        n += r;

    close(p[0]);

    int st;
    waitpid(pid, &st, 0);

    extern char *__progname;
    char want[256];
    snprintf(want, sizeof(want), "%s: file 7: No such file or directory\n%s: plain\n%s: fatal: Permission denied\n",
             __progname, __progname, __progname);

    check("err: exit status", WEXITSTATUS(st), 3);
    check("warn, warnx, err: the messages", !strcmp(buf, want), 1);
    if (strcmp(buf, want))
        fprintf(out, "    got: %s", buf);
}

static void *pt_key_seen;

static void pt_once_init(void) { pt_once_runs++; }

static void *pt_thread(void *arg)
{
    pt_self = pthread_self();
    pthread_setspecific(pt_key, arg);
    pt_key_seen = pthread_getspecific(pt_key);

    for (int i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&pt_lock);
        int v = pt_count;
        if (i % 100 == 0)
            usleep(100);    /* the other thread tries meanwhile */
        pt_count = v + 1;
        pthread_mutex_unlock(&pt_lock);
    }

    pthread_mutex_lock(&pt_lock);
    pt_ready = 1;
    pthread_cond_signal(&pt_cond);
    pthread_mutex_unlock(&pt_lock);
    return arg;
}

/* signals and threads: each thread's own mask, a process's signal taken
 * by a thread that doesn't block it, pthread_kill(), sigwait(), and
 * detached threads */
static volatile pthread_t ps_handled_by;
static volatile int ps_handled, ps_go, ps_done, ps_waited, ps_blocked_in_thread;

static void ps_handler(int sig)
{
    (void) sig;
    ps_handled_by = pthread_self();
    ps_handled++;
}

/* the mask it started with (its creator's), then waits until told */
static void *ps_thread(void *arg)
{
    sigset_t mask;

    pthread_sigmask(SIG_BLOCK, NULL, &mask);
    ps_blocked_in_thread = sigismember(&mask, SIGUSR1);

    if (arg) {
        /* takes SIGUSR1 from here on */
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &one, NULL);
    }

    while (!ps_go)
        usleep(1000);

    return NULL;
}

static void *ps_waiter(void *arg)
{
    (void) arg;
    sigset_t set;
    int sig = 0;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    ps_waited = sigwait(&set, &sig) == 0 ? sig : -1;
    return NULL;
}

static void *ps_short(void *arg)
{
    (void) arg;
    ps_done = 1;
    return NULL;
}

/* a thread that writes to a pipe every millisecond, in a child that is
 * stopped and continued */
static void *ps_ticker(void *arg)
{
    int fd = (int) (long) arg;

    for (;;) {
        write(fd, "t", 1);
        usleep(1000);
    }

    return NULL;
}

/* what the pipe has now, without waiting */
static int ps_drain(int fd)
{
    char buf[256];
    int n = 0, r;

    while ((r = read(fd, buf, sizeof(buf))) > 0)
        n += r;

    return n;
}

/* siginfo, ucontext and sigaltstack: who sent a signal, why a fault
 * came, a handler that changes the context it returns to, a child's
 * SIGCHLD, a handler on its own stack */
static siginfo_t si_got;
static volatile int si_count, si_onstack, si_onstack_flags, si_altstack_eperm;
static volatile int si_resumed, si_pc_is_addr, si_mask_had;
static char *volatile si_local;

static void si_handler(int sig, siginfo_t *info, void *ctx)
{
    (void) sig;
    (void) ctx;
    si_got = *info;
    si_count++;
}

/* SIGILL from a trap instruction: the siginfo's address is the
 * ucontext's pc, and the handler goes on past the instruction */
static void si_ill_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = ctx;

    (void) sig;
    si_got = *info;
#if defined(__x86_64__)
    si_pc_is_addr = info->si_addr == (void *) uc->uc_mcontext.rip;
    uc->uc_mcontext.rip += 2;               /* ud2 */
#elif defined(__i386__)
    si_pc_is_addr = info->si_addr == (void *) uc->uc_mcontext.eip;
    uc->uc_mcontext.eip += 2;
#else
    si_pc_is_addr = info->si_addr == (void *) uc->uc_mcontext.pc;
    uc->uc_mcontext.pc += 4;                /* udf */
#endif
}

/* the mask it returns with: the ucontext's, which it changes */
static void si_mask_handler(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = ctx;

    (void) sig;
    (void) info;
    si_mask_had = sigismember(&uc->uc_sigmask, SIGTERM) && !sigismember(&uc->uc_sigmask, SIGUSR2);
    sigaddset(&uc->uc_sigmask, SIGUSR1);
}

/* on the alternate stack: where it runs, what sigaltstack() says, and
 * that the stack can't be changed meanwhile */
static void si_stack_handler(int sig, siginfo_t *info, void *ctx)
{
    char here;
    stack_t ss, other = {0};

    (void) sig;
    (void) info;
    (void) ctx;
    si_local = &here;
    sigaltstack(NULL, &ss);
    si_onstack_flags = ss.ss_flags;
    other.ss_flags = SS_DISABLE;
    si_altstack_eperm = sigaltstack(&other, NULL) == -1 && errno == EPERM;
    si_onstack++;
}

static void si_overflow_handler(int sig)
{
    (void) sig;
    _exit(42);
}

static int si_recurse(int n)
{
    volatile char frame[4096];

    frame[0] = n;
    return si_recurse(n + 1) + frame[0];
}

static void *si_thread_altstack(void *arg)
{
    stack_t ss;

    sigaltstack(NULL, &ss);
    *(int *) arg = ss.ss_flags;
    return NULL;
}

static void *si_raiser(void *arg)
{
    (void) arg;
    raise(SIGUSR1);
    return NULL;
}

/* waits until the SIGCHLD handler ran `n` times */
static void si_wait_count(int n)
{
    for (int i = 0; i < 500 && si_count < n; ++i)
        usleep(1000);
}

static void test_siginfo(void)
{
    struct sigaction sa = {0}, dfl = {0};
    sa.sa_sigaction = si_handler;
    sa.sa_flags = SA_SIGINFO;
    dfl.sa_handler = SIG_DFL;

    /* sent by kill(): the sender's pid and user id */
    sigaction(SIGUSR1, &sa, NULL);
    si_count = 0;
    kill(getpid(), SIGUSR1);
    check("siginfo: kill() is SI_USER, from us",
          si_count == 1 && si_got.si_signo == SIGUSR1 && si_got.si_code == SI_USER &&
          si_got.si_pid == getpid() && si_got.si_uid == getuid(), 1);

    /* sent by the kernel: a pipe with no reader */
    sigaction(SIGPIPE, &sa, NULL);
    int pp[2];
    pipe(pp);
    close(pp[0]);
    si_count = 0;
    write(pp[1], "x", 1);
    close(pp[1]);
    check("siginfo: SIGPIPE is SI_KERNEL", si_count == 1 && si_got.si_code == SI_KERNEL, 1);
    sigaction(SIGPIPE, &dfl, NULL);

    /* faults: an address with no mapping, then a read-only one */
    struct sigaction fsa = {0};
    fsa.sa_sigaction = fault_handler;
    fsa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &fsa, NULL);
    int jumped = sigsetjmp(fault_jmp, 1);
    if (!jumped)
        *(volatile int *) 0x1234 = 1;
    check("siginfo: SEGV_MAPERR", jumped == SIGSEGV && fault_code == SEGV_MAPERR && fault_addr == (void *) 0x1234, 1);

    char *ro = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    jumped = sigsetjmp(fault_jmp, 1);
    if (!jumped)
        ro[8] = 1;
    check("siginfo: SEGV_ACCERR", ro != MAP_FAILED && jumped == SIGSEGV &&
          fault_code == SEGV_ACCERR && fault_addr == ro + 8, 1);
    munmap(ro, 4096);
    sigaction(SIGSEGV, &dfl, NULL);

#if defined(__x86_64__) || defined(__i386__)
    /* an integer division by zero */
    sigaction(SIGFPE, &fsa, NULL);
    volatile int zero = 0, quot = 1;
    jumped = sigsetjmp(fault_jmp, 1);
    if (!jumped)
        quot = 7 / zero;
    check("siginfo: FPE_INTDIV", jumped == SIGFPE && fault_code == FPE_INTDIV && quot == 1, 1);
    sigaction(SIGFPE, &dfl, NULL);
#endif

    /* a trap instruction: the handler moves the pc past it, and the
     * program goes on from there */
    struct sigaction isa = {0};
    isa.sa_sigaction = si_ill_handler;
    isa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &isa, NULL);
    si_resumed = 0;
#if defined(__x86_64__) || defined(__i386__)
    asm volatile("ud2");
    si_resumed = 1;
    check("ucontext: SIGILL's ILL_ILLOPN at the pc, and on past it",
          si_got.si_code == ILL_ILLOPN && si_pc_is_addr && si_resumed, 1);
#else
    asm volatile("udf #0");
    si_resumed = 1;
    check("ucontext: SIGILL's ILL_ILLOPC at the pc, and on past it",
          si_got.si_code == ILL_ILLOPC && si_pc_is_addr && si_resumed, 1);
#endif
    sigaction(SIGILL, &dfl, NULL);

    /* the mask: the one before the handler, which it changes */
    sigset_t term, now;
    sigemptyset(&term);
    sigaddset(&term, SIGTERM);
    sigprocmask(SIG_BLOCK, &term, NULL);
    struct sigaction msa = {0};
    msa.sa_sigaction = si_mask_handler;
    msa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &msa, NULL);
    raise(SIGUSR2);
    sigprocmask(SIG_BLOCK, NULL, &now);
    check("ucontext: uc_sigmask is the mask before, and comes back changed",
          si_mask_had && sigismember(&now, SIGUSR1) && sigismember(&now, SIGTERM), 1);
    sigaddset(&term, SIGUSR1);
    sigprocmask(SIG_UNBLOCK, &term, NULL);
    sigaction(SIGUSR2, &dfl, NULL);

    /* sigtimedwait's siginfo */
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    sigprocmask(SIG_BLOCK, &usr1, NULL);
    kill(getpid(), SIGUSR1);
    siginfo_t wi = {0};
    check("siginfo: sigwaitinfo's", sigwaitinfo(&usr1, &wi) == SIGUSR1 &&
          wi.si_code == SI_USER && wi.si_pid == getpid(), 1);
    sigprocmask(SIG_UNBLOCK, &usr1, NULL);

    /* raise() in a thread: that thread takes it, not the first one that
     * doesn't block it */
    struct sigaction rsa = {0};
    rsa.sa_handler = ps_handler;
    sigaction(SIGUSR1, &rsa, NULL);
    pthread_t t;
    ps_handled = 0;
    pthread_create(&t, NULL, si_raiser, NULL);
    pthread_join(t, NULL);
    check("raise: in a thread, to that thread", ps_handled == 1 && pthread_equal(ps_handled_by, t), 1);
    sigaction(SIGUSR1, &dfl, NULL);

    /* SIGCHLD: how each child changed */
    sigaction(SIGCHLD, &sa, NULL);
    int st;
    si_count = 0;
    pid_t c = fork();
    if (!c)
        _exit(7);
    si_wait_count(1);
    check("siginfo: SIGCHLD CLD_EXITED", si_got.si_code == CLD_EXITED && si_got.si_pid == c &&
          si_got.si_status == 7, 1);
    waitpid(c, &st, 0);

    si_count = 0;
    c = fork();
    if (!c)
        for (;;)
            pause();
    kill(c, SIGSTOP);
    si_wait_count(1);
    check("siginfo: SIGCHLD CLD_STOPPED", si_got.si_code == CLD_STOPPED && si_got.si_pid == c &&
          si_got.si_status == SIGSTOP, 1);
    waitpid(c, &st, WUNTRACED);
    kill(c, SIGCONT);
    si_wait_count(2);
    check("siginfo: SIGCHLD CLD_CONTINUED", si_got.si_code == CLD_CONTINUED && si_got.si_pid == c, 1);
    kill(c, SIGTERM);
    si_wait_count(3);
    check("siginfo: SIGCHLD CLD_KILLED", si_got.si_code == CLD_KILLED && si_got.si_pid == c &&
          si_got.si_status == SIGTERM, 1);
    waitpid(c, &st, 0);
    sigaction(SIGCHLD, &dfl, NULL);

    /* sigaltstack: none at first, then a handler with SA_ONSTACK runs on
     * it, and the stack can't change meanwhile */
    stack_t ss = {0}, old;
    check("sigaltstack: none at first", sigaltstack(NULL, &old) == 0 && old.ss_flags == SS_DISABLE, 1);
    ss.ss_sp = malloc(SIGSTKSZ);
    ss.ss_size = SIGSTKSZ;
    check("sigaltstack: set", sigaltstack(&ss, NULL), 0);
    check("sigaltstack: set, not on it", sigaltstack(NULL, &old) == 0 && old.ss_flags == 0 &&
          old.ss_sp == ss.ss_sp && old.ss_size == SIGSTKSZ, 1);
    struct sigaction osa = {0};
    osa.sa_sigaction = si_stack_handler;
    osa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGUSR2, &osa, NULL);
    si_onstack = 0;
    raise(SIGUSR2);
    check("sigaltstack: the handler runs on it", si_onstack == 1 &&
          si_local > (char *) ss.ss_sp && si_local < (char *) ss.ss_sp + SIGSTKSZ, 1);
    check("sigaltstack: SS_ONSTACK in the handler, EPERM to change it",
          si_onstack_flags == SS_ONSTACK && si_altstack_eperm, 1);
    osa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &osa, NULL);
    raise(SIGUSR2);
    check("sigaltstack: not without SA_ONSTACK", si_onstack == 2 &&
          !(si_local > (char *) ss.ss_sp && si_local < (char *) ss.ss_sp + SIGSTKSZ), 1);
    sigaction(SIGUSR2, &dfl, NULL);

    int tflags = 0;
    pthread_create(&t, NULL, si_thread_altstack, &tflags);
    pthread_join(t, NULL);
    check("sigaltstack: a new thread has none", tflags, SS_DISABLE);

    stack_t bad = {ss.ss_sp, 0, MINSIGSTKSZ - 1};
    errno = 0;
    check("sigaltstack: too small -> ENOMEM", sigaltstack(&bad, NULL) == -1 && errno == ENOMEM, 1);
    bad.ss_size = SIGSTKSZ;
    bad.ss_flags = 0x100;
    errno = 0;
    check("sigaltstack: bad flags -> EINVAL", sigaltstack(&bad, NULL) == -1 && errno == EINVAL, 1);
    stack_t off = {0};
    off.ss_flags = SS_DISABLE;
    check("sigaltstack: disabled", sigaltstack(&off, NULL) == 0 && sigaltstack(NULL, &old) == 0 &&
          old.ss_flags == SS_DISABLE, 1);

    /* a stack overflow: SIGSEGV's handler can only run on another stack */
    for (int on = 0; on < 2; ++on) {
        c = fork();
        if (!c) {
            if (on)
                sigaltstack(&ss, NULL);
            struct sigaction vsa = {0};
            vsa.sa_handler = si_overflow_handler;
            vsa.sa_flags = SA_ONSTACK;
            sigaction(SIGSEGV, &vsa, NULL);
            _exit(si_recurse(0));
        }
        waitpid(c, &st, 0);
        if (on)
            check("sigaltstack: a stack overflow's handler runs", WIFEXITED(st) && WEXITSTATUS(st) == 42, 1);
        else
            check("sigaltstack: without one, the overflow kills", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, 1);
    }

    free(ss.ss_sp);
    sigaction(SIGUSR1, &dfl, NULL);
}

static void test_pthread_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = ps_handler;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    sigset_t usr1, old, pending;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);

    /* the main thread blocks SIGUSR1; a new thread starts blocking it
     * too, then unblocks it for itself alone */
    pthread_sigmask(SIG_BLOCK, &usr1, &old);
    pthread_t t;
    ps_go = 0;
    check("signals: pthread_create", pthread_create(&t, NULL, ps_thread, (void *) 1), 0);
    usleep(20000);
    check("signals: a new thread has its creator's mask", ps_blocked_in_thread, 1);

    sigset_t now;
    pthread_sigmask(SIG_BLOCK, NULL, &now);
    check("signals: the thread's unblock isn't the main thread's", sigismember(&now, SIGUSR1), 1);

    /* sent to the process: the thread that doesn't block it takes it */
    ps_handled = 0;
    kill(getpid(), SIGUSR1);
    for (int i = 0; i < 200 && !ps_handled; ++i)
        usleep(1000);
    check("signals: kill() taken by the thread not blocking it",
          ps_handled == 1 && pthread_equal(ps_handled_by, t), 1);

    /* sent to a thread: that one takes it */
    ps_handled = 0;
    check("signals: pthread_kill", pthread_kill(t, SIGUSR2), 0);
    for (int i = 0; i < 200 && !ps_handled; ++i)
        usleep(1000);
    check("signals: pthread_kill taken by that thread",
          ps_handled == 1 && pthread_equal(ps_handled_by, t), 1);

    /* sent to the main thread, which blocks it: pending for it alone */
    ps_handled = 0;
    pthread_kill(pthread_self(), SIGUSR1);
    usleep(20000);
    sigpending(&pending);
    check("signals: blocked pthread_kill stays pending on that thread",
          !ps_handled && sigismember(&pending, SIGUSR1), 1);
    sigset_t got_set = usr1;
    int got = 0;
    check("signals: sigwait takes it", sigwait(&got_set, &got) == 0 && got == SIGUSR1 && !ps_handled, 1);

    ps_go = 1;
    pthread_join(t, NULL);

    check("signals: pthread_kill bad signal", pthread_kill(pthread_self(), 99), EINVAL);
    check("signals: pthread_kill gone thread", pthread_kill(t, 0), ESRCH);

    /* sigwait in a thread: a process's signal goes to it, not to a
     * handler, though every thread blocks it */
    ps_waited = 0;
    ps_handled = 0;
    pthread_create(&t, NULL, ps_waiter, NULL);
    usleep(20000);
    kill(getpid(), SIGUSR1);
    pthread_join(t, NULL);
    check("signals: sigwait in a thread gets kill()'s signal", ps_waited == SIGUSR1 && !ps_handled, 1);

    /* sigtimedwait: the time runs out, or the signal is there */
    struct timespec ts = {0, 50000000};
    siginfo_t si;
    errno = 0;
    check("signals: sigtimedwait times out", sigtimedwait(&usr1, &si, &ts) == -1 && errno == EAGAIN, 1);
    kill(getpid(), SIGUSR1);
    memset(&si, 0, sizeof(si));
    check("signals: sigwaitinfo", sigwaitinfo(&usr1, &si) == SIGUSR1 && si.si_signo == SIGUSR1, 1);
    ts.tv_nsec = 1000000000;
    errno = 0;
    check("signals: sigtimedwait bad time", sigtimedwait(&usr1, &si, &ts) == -1 && errno == EINVAL, 1);

    pthread_sigmask(SIG_SETMASK, &old, NULL);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* detached threads: freed as soon as they exit, whether detached
     * before or after; no one joins them */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ps_done = 0;
    check("detach: created detached", pthread_create(&t, &attr, ps_short, NULL), 0);
    for (int i = 0; i < 200 && !ps_done; ++i)
        usleep(1000);
    usleep(20000);
    check("detach: gone once it exits", pthread_join(t, NULL), ESRCH);

    ps_go = 0;
    pthread_create(&t, NULL, ps_thread, NULL);
    check("detach: a running thread", pthread_detach(t), 0);
    check("detach: twice", pthread_detach(t), EINVAL);
    check("detach: join a detached thread", pthread_join(t, NULL), EINVAL);
    ps_go = 1;
    usleep(50000);
    check("detach: gone once it ends", pthread_join(t, NULL), ESRCH);

    ps_done = 0;
    pthread_create(&t, NULL, ps_short, NULL);
    for (int i = 0; i < 200 && !ps_done; ++i)
        usleep(1000);
    usleep(20000);
    check("detach: one that has ended", pthread_detach(t) == 0 && pthread_join(t, NULL) == ESRCH, 1);

    /* threads' stacks are freed: made and joined, or detached, over and
     * over, the heap stays put */
    char *brk0 = sbrk(0);
    for (int i = 0; i < 300; ++i) {
        pthread_create(&t, i % 2 ? &attr : NULL, ps_short, NULL);
        if (i % 2 == 0)
            pthread_join(t, NULL);
        else
            usleep(2000);
    }
    usleep(50000);
    pthread_create(&t, NULL, ps_short, NULL);
    pthread_join(t, NULL);
    check("threads: their stacks freed (heap growth under 256 KiB)", (char *) sbrk(0) - brk0 < 256 * 1024, 1);
    pthread_attr_destroy(&attr);

    /* stopping a process stops all its threads: the ticker of a stopped
     * child writes nothing until it's continued */
    int tp[2];
    pipe(tp);
    pid_t c = fork();
    if (!c) {
        close(tp[0]);
        pthread_t tick;
        pthread_create(&tick, NULL, ps_ticker, (void *) (long) tp[1]);
        for (;;)
            pause();
    }
    close(tp[1]);
    fcntl(tp[0], F_SETFL, O_NONBLOCK);
    usleep(50000);
    int before = ps_drain(tp[0]);
    int st;
    kill(c, SIGSTOP);
    waitpid(c, &st, WUNTRACED);
    usleep(20000);
    ps_drain(tp[0]);
    usleep(100000);
    int stopped = ps_drain(tp[0]);
    kill(c, SIGCONT);
    usleep(50000);
    int after = ps_drain(tp[0]);
    kill(c, SIGKILL);
    waitpid(c, &st, 0);
    close(tp[0]);
    check("stop: every thread stops, and goes on when continued",
          before > 0 && WIFSIGNALED(st) && stopped == 0 && after > 0, 1);
}

/* a thread that runs off the end of its stack: the guard page below
 * it faults (SEGV_ACCERR, where nothing mapped would be SEGV_MAPERR),
 * and the handler, on the thread's alternate stack, says so */
static void guard_handler(int sig, siginfo_t *info, void *ctx)
{
    _exit(info->si_code == SEGV_ACCERR ? 42 : 1);
}

static void *guard_none(void *arg)
{
    return arg;
}

static int deeper(int n)
{
    volatile char buf[512];
    buf[0] = n;
    return deeper(n + 1) + buf[0];
}

static void *guard_thread(void *arg)
{
    static char alt[8192];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof(alt), .ss_flags = 0};
    sigaltstack(&ss, NULL);
    return (void *) (long) deeper(0);
}

/* mprotect(), pread() and pwrite(), pipe2(), dup3(), socketpair(),
 * sched_yield(), clock_nanosleep(), and threads' guard pages */
static void test_posix_calls(void)
{
    /* a page of three made read-only: a write faults, the others don't */
    char *m = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    m[4096] = 'm';
    check("mprotect", mprotect(m + 4096, 4096, PROT_READ), 0);
    struct sigaction fsa = {0}, dfl = {0};
    fsa.sa_sigaction = fault_handler;
    fsa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &fsa, NULL);
    int jumped = sigsetjmp(fault_jmp, 1);
    if (!jumped)
        m[4096 + 5] = 1;
    check("mprotect: a write faults, SEGV_ACCERR", jumped == SIGSEGV && fault_code == SEGV_ACCERR &&
          fault_addr == m + 4096 + 5, 1);
    sigaction(SIGSEGV, &dfl, NULL);
    m[0] = m[2 * 4096] = 1;
    check("mprotect back", mprotect(m, 3 * 4096, PROT_READ | PROT_WRITE), 0);
    m[4096 + 5] = 1;
    check("mprotect back: written, kept", m[4096] == 'm' && m[4096 + 5] == 1, 1);
    errno = 0;
    check("mprotect unaligned -> EINVAL", mprotect(m + 1, 4096, PROT_READ) == -1 ? errno : 0, EINVAL);
    check("munmap a page of it", munmap(m + 4096, 4096), 0);
    errno = 0;
    check("mprotect a hole -> ENOMEM", mprotect(m, 3 * 4096, PROT_READ) == -1 ? errno : 0, ENOMEM);
    munmap(m, 3 * 4096);

    /* pread() and pwrite() don't move the offset */
    char b[16] = {0};
    int f = open("/tmp/at", O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(f, "0123456789", 10);
    lseek(f, 3, SEEK_SET);
    check("pread", pread(f, b, 2, 6) == 2 && b[0] == '6' && b[1] == '7', 1);
    check("pwrite", pwrite(f, "xy", 2, 0), 2);
    check("pread and pwrite: the offset stays", lseek(f, 0, SEEK_CUR), 3);
    check("read after them", read(f, b, 3) == 3 && b[0] == '3', 1);
    errno = 0;
    check("pread negative -> EINVAL", pread(f, b, 1, -5) == -1 ? errno : 0, EINVAL);
    close(f);
    unlink("/tmp/at");
    int p[2];
    pipe(p);
    errno = 0;
    check("pread pipe -> ESPIPE", pread(p[0], b, 1, 0) == -1 ? errno : 0, ESPIPE);
    close(p[0]);
    close(p[1]);

    /* pipe2(), dup3(), and dup2() over an open descriptor */
    check("pipe2", pipe2(p, O_CLOEXEC | O_NONBLOCK), 0);
    check("pipe2 O_CLOEXEC", fcntl(p[0], F_GETFD), FD_CLOEXEC);
    errno = 0;
    check("pipe2 O_NONBLOCK", read(p[0], b, 1) == -1 ? errno : 0, EAGAIN);
    check("dup3", dup3(p[1], 30, O_CLOEXEC), 30);
    check("dup3 O_CLOEXEC", fcntl(30, F_GETFD), FD_CLOEXEC);
    errno = 0;
    check("dup3 same -> EINVAL", dup3(30, 30, 0) == -1 ? errno : 0, EINVAL);
    check("dup2 same", dup2(30, 30), 30);
    int q[2];
    pipe(q);
    check("dup2 over an open descriptor", dup2(q[1], 30), 30);
    check("dup2: no O_CLOEXEC", fcntl(30, F_GETFD), 0);
    write(30, "q", 1);
    check("dup2: the new file", read(q[0], b, 1) == 1 && b[0] == 'q', 1);
    for (int i = 0; i < 2; ++i) {
        close(p[i]);
        close(q[i]);
    }
    close(30);

    /* socketpair(), its flags set by the kernel */
    int sv[2];
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
    check("socketpair SOCK_CLOEXEC", fcntl(sv[0], F_GETFD) == FD_CLOEXEC && fcntl(sv[1], F_GETFD) == FD_CLOEXEC, 1);
    check("socketpair send and recv", send(sv[1], "pair", 4, 0) == 4 && recv(sv[0], b, 16, 0) == 4 &&
          !memcmp(b, "pair", 4), 1);
    close(sv[0]);
    close(sv[1]);

    check("sched_yield", sched_yield(), 0);

    /* clock_nanosleep() returns the error, errno stays */
    struct timespec ts = {0, 20000000}, t0, t1;
    errno = 0;
    check("clock_nanosleep bad clock: the error returned", clock_nanosleep(99, 0, &ts, NULL), EINVAL);
    check("clock_nanosleep: errno untouched", errno, 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check("clock_nanosleep", clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    check("clock_nanosleep: slept", (t1.tv_sec - t0.tv_sec) * 1000000000L + t1.tv_nsec - t0.tv_nsec >= 20000000, 1);
    ts = t1;
    ts.tv_nsec += 20000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    check("clock_nanosleep TIMER_ABSTIME", clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    check("clock_nanosleep TIMER_ABSTIME: then", t1.tv_sec > ts.tv_sec ||
          (t1.tv_sec == ts.tv_sec && t1.tv_nsec >= ts.tv_nsec), 1);

    /* a thread's stack has a guard page */
    pid_t c = fork();
    if (!c) {
        struct sigaction gsa = {0};
        gsa.sa_sigaction = guard_handler;
        gsa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &gsa, NULL);
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16384);
        pthread_t t;
        pthread_create(&t, &attr, guard_thread, NULL);
        pthread_join(t, NULL);
        _exit(2);
    }
    int st = 0;
    waitpid(c, &st, 0);
    check("pthread: a stack overflow hits the guard page", WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st), 42);

    /* stacks are mapped and unmapped: many threads, one after another */
    int ok = 1;
    for (int i = 0; i < 200 && ok; ++i) {
        pthread_t t;
        ok = !pthread_create(&t, NULL, guard_none, NULL) && !pthread_join(t, NULL);
    }
    check("pthread: 200 threads made and joined", ok, 1);
}

static void test_pthread(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, pt_once_init);
    pthread_once(&once, pt_once_init);
    check("pthread_once", pt_once_runs, 1);

    check("pthread_key_create", pthread_key_create(&pt_key, NULL), 0);
    pthread_setspecific(pt_key, (void *) 1);

    pthread_t t;
    check("pthread_create", pthread_create(&t, NULL, pt_thread, (void *) 2), 0);

    for (int i = 0; i < 2000; ++i) {
        pthread_mutex_lock(&pt_lock);
        int v = pt_count;
        if (i % 100 == 0)
            usleep(100);
        pt_count = v + 1;
        pthread_mutex_unlock(&pt_lock);
    }

    pthread_mutex_lock(&pt_lock);
    while (!pt_ready)
        pthread_cond_wait(&pt_cond, &pt_lock);
    pthread_mutex_unlock(&pt_lock);

    void *ret = NULL;
    check("pthread_join", pthread_join(t, &ret), 0);
    check("thread's result", (long) ret, 2);
    check("mutex kept the count", pt_count, 4000);
    check("pthread_self of the thread", pthread_equal(pt_self, t), 1);
    check("pthread_self differs", pthread_equal(pthread_self(), t), 0);
    check("key value per thread", (long) pt_key_seen * 10 + (long) pthread_getspecific(pt_key), 21);
    check("trylock held -> EBUSY", pthread_mutex_lock(&pt_lock) == 0 && pthread_mutex_trylock(&pt_lock) == EBUSY, 1);
    pthread_mutex_unlock(&pt_lock);

    struct timespec past = {0, 0};
    pthread_mutex_lock(&pt_lock);
    check("cond_timedwait past -> ETIMEDOUT", pthread_cond_timedwait(&pt_cond, &pt_lock, &past), ETIMEDOUT);
    pthread_mutex_unlock(&pt_lock);

    /* the reentrant functions of <pwd.h> and <dirent.h> */
    struct passwd pw, *pwp = NULL;
    char pwbuf[256];
    check("getpwuid_r", getpwuid_r(0, &pw, pwbuf, sizeof(pwbuf), &pwp) == 0 && pwp == &pw && !strcmp(pw.pw_name, "root"), 1);
    check("getpwuid_r small -> ERANGE", getpwuid_r(0, &pw, pwbuf, 3, &pwp), ERANGE);
    check("getpwnam_r missing", getpwnam_r("nobody-here", &pw, pwbuf, sizeof(pwbuf), &pwp) == 0 && pwp == NULL, 1);

    /* pseudo-terminals, as xterm opens them */
    struct { char *dev, *opt; } pts_args = {0, 0};
    mknod("/dev/ptmx", S_IFCHR | 0666, (5 << 8) | 2);
    mkdir("/dev/pts", 0755);
    mount("devpts", "/dev/pts", 0, &pts_args);
    int ptm = posix_openpt(O_RDWR | O_NOCTTY);
    check("posix_openpt", ptm >= 0, 1);
    check("grantpt", grantpt(ptm), 0);
    check("unlockpt", unlockpt(ptm), 0);
    char *pts = ptsname(ptm);
    check("ptsname", pts && !strncmp(pts, "/dev/pts/", 9), 1);
    char ptsbuf[4];
    check("ptsname_r small -> ERANGE", ptsname_r(ptm, ptsbuf, sizeof(ptsbuf)), ERANGE);
    int ptsfd = pts ? open(pts, O_RDWR | O_NOCTTY) : -1;
    char ptc = 0;
    check("pty slave", ptsfd >= 0 && write(ptm, "x\n", 2) == 2 && read(ptsfd, &ptc, 1) == 1 && ptc == 'x', 1);
    close(ptsfd);
    close(ptm);
    errno = 0;
    check("grantpt not a master -> EINVAL", grantpt(0) == -1 ? errno : 0, EINVAL);

    /* openpty(), as st uses it, with the window size set */
    int opm = -1, ops = -1;
    char opname[32] = "";
    struct winsize ows = {24, 80, 0, 0}, gws = {0};
    check("openpty", openpty(&opm, &ops, opname, NULL, &ows), 0);
    check("openpty name", !strncmp(opname, "/dev/pts/", 9), 1);
    check("openpty winsize", ioctl(ops, TIOCGWINSZ, &gws) == 0 && gws.ws_row == 24 && gws.ws_col == 80, 1);
    check("openpty slave", write(opm, "y\n", 2) == 2 && read(ops, &ptc, 1) == 1 && ptc == 'y', 1);
    close(ops);
    close(opm);

    /* getentropy(): arc4random() kills the process if it fails */
    unsigned char ent[32] = {0}, ent2[32] = {0};
    check("getentropy", getentropy(ent, sizeof(ent)), 0);
    getentropy(ent2, sizeof(ent2));
    check("getentropy differs", memcmp(ent, ent2, sizeof(ent)) != 0, 1);
    errno = 0;
    check("getentropy > 256 -> EIO", getentropy(ent, 257) == -1 ? errno : 0, EIO);
    check("arc4random", arc4random() != arc4random() || arc4random() != arc4random(), 1);

    /* setpgrp(): a group of its own */
    pid_t pg = fork();
    if (!pg)
        _exit(setpgrp() == 0 && getpgrp() == getpid() ? 0 : 1);
    int pgst;
    waitpid(pg, &pgst, 0);
    check("setpgrp", WEXITSTATUS(pgst), 0);

    DIR *d = opendir("/");
    struct dirent de, *dep = NULL;
    int entries = 0;
    while (readdir_r(d, &de, &dep) == 0 && dep)
        entries++;
    closedir(d);
    check("readdir_r", entries > 2, 1);
}

static int vt, vcs;

static void vt_write(const char *s)
{
    write(vt, s, strlen(s));
}

/* a row of the screen, without trailing blanks */
static const char *vt_row(int row)
{
    static char buf[81];

    lseek(vcs, row * 80, SEEK_SET);
    buf[read(vcs, buf, 80) < 80 ? 0 : 80] = '\0';

    for (int i = 79; i >= 0 && buf[i] == ' '; --i)
        buf[i] = '\0';

    return buf;
}

static void test_vt(void)
{
    mknod("/dev/tty2", S_IFCHR | 0600, (4 << 8) | 2);
    mknod("/dev/vcs2", S_IFCHR | 0600, (7 << 8) | 2);
    mknod("/dev/vcsa2", S_IFCHR | 0600, (7 << 8) | 130);

    vt = open("/dev/tty2", O_RDWR | O_NOCTTY);
    vcs = open("/dev/vcs2", O_RDONLY);
    check("vt open", vt >= 0 && vcs >= 0, 1);

    struct winsize ws = {0};
    ioctl(vt, TIOCGWINSZ, &ws);
    check("vt size", ws.ws_row * 1000 + ws.ws_col, 25080);

    /* cursor addressing, colors */
    vt_write("\033[H\033[2Jhello\n\033[3;5Hab\033[1;31mc\033[0m");
    check("vt text", strcmp(vt_row(0), "hello"), 0);
    check("vt cup", strcmp(vt_row(2), "    abc"), 0);

    /* vcsa: rows, columns, cursor column and row, then characters with
     * VGA attributes (bold red is light red, 12) */
    unsigned char vcsa[4 + 2 * 80 * 3];
    int fd = open("/dev/vcsa2", O_RDONLY);
    check("vcsa read", read(fd, vcsa, sizeof(vcsa)), sizeof(vcsa));
    close(fd);
    check("vcsa header", vcsa[0] << 24 | vcsa[1] << 16 | vcsa[2] << 8 | vcsa[3], 25 << 24 | 80 << 16 | 7 << 8 | 2);
    check("vcsa attr", vcsa[4 + 2 * (2 * 80 + 6) + 1], 0x0c);

    /* insert and erase characters, erase to the end of the line */
    vt_write("\033[5;1Habcdef\033[3D\033[1@X\033[K|");
    check("vt ich el", strcmp(vt_row(4), "abcX|"), 0);
    vt_write("\033[6;1Habcdef\033[1;3H\033[6;2H\033[2P\033[1X");
    check("vt dch ech", strcmp(vt_row(5), "a ef"), 0);

    /* DEC line drawing, UTF-8 to code page 437, REP */
    vt_write("\033[7;1H\033(0lqk\033(B caf\303\251 \342\224\200x\033[2b \342\231\245\342\214\202");
    check("vt charsets", strcmp(vt_row(6), "\xda\xc4\xbf caf\x82 \xc4xxx \x03\x7f"), 0);

    /* lines scroll within the scrolling region */
    vt_write("\033[9;1Htop\033[10;12r\033[10;1HA\033[11;1HB\033[12;1HC\n\033[r");
    check("vt region", strcmp(vt_row(9), "B") || strcmp(vt_row(10), "C") ||
                       *vt_row(11) || strcmp(vt_row(8), "top"), 0);

    /* inserting and deleting lines */
    vt_write("\033[10;1H\033[L");
    check("vt il", *vt_row(9) || strcmp(vt_row(10), "B"), 0);
    vt_write("\033[M\033[M");
    check("vt dl", strcmp(vt_row(9), "C"), 0);

    /* the last column wraps on the next character */
    vt_write("\033[15;78H12345");
    check("vt wrap", strcmp(vt_row(14) + 77, "123") || strcmp(vt_row(15), "45"), 0);

    /* the alternate screen, the main one comes back */
    vt_write("\033[?1049h\033[Halt");
    check("vt alt screen", strcmp(vt_row(0), "alt"), 0);
    vt_write("\033[?1049l");
    check("vt main screen", strcmp(vt_row(0), "hello"), 0);

    /* a cursor position report is input */
    struct termios tios;
    tcgetattr(vt, &tios);
    cfmakeraw(&tios);
    tios.c_cc[VMIN] = 0;
    tios.c_cc[VTIME] = 10;
    tcsetattr(vt, TCSANOW, &tios);

    char reply[16] = {0};
    vt_write("\033[3;5H\033[6n");
    read(vt, reply, sizeof(reply) - 1);
    check("vt dsr", strcmp(reply, "\033[3;5R"), 0);

    close(vcs);
    close(vt);
}

extern void _start(void);

/* scandir()'s filter: no dot entries */
static int no_dots(const struct dirent *d)
{
    return d->d_name[0] != '.';
}

/* scandir() of a directory with known entries: filtered, sorted, all
 * freed */
static int scandir_sorted(void)
{
    mkdir("/tmp/sd", 0755);

    const char *names[] = {"c", "a", "b"};

    for (int i = 0; i < 3; ++i) {
        char path[16] = "/tmp/sd/";
        strcat(path, names[i]);
        close(open(path, O_CREAT | O_WRONLY, 0644));
    }

    struct dirent **list;
    int n = scandir("/tmp/sd", &list, no_dots, alphasort);
    int ok = n == 3 && !strcmp(list[0]->d_name, "a") && !strcmp(list[1]->d_name, "b") &&
             !strcmp(list[2]->d_name, "c");

    for (int i = 0; i < n; ++i)
        free(list[i]);

    if (n >= 0)
        free(list);

    return ok;
}

int main(void)
{
    struct { char *dev, *opt; } no_args = {0, 0};

    mount("devfs", "/dev", 0, &no_args);
    mknod("/dev/ttyS0", S_IFCHR | 0666, (4 << 8) | 64);
    out = fopen("/dev/ttyS0", "w");
    setvbuf(out, NULL, _IONBF, 0);
    fprintf(out, "aqtest: libc start\n");

    /* the auxiliary vector, through crt0 and getauxval() */
    check("getauxval(AT_PAGESZ)", getauxval(AT_PAGESZ), 4096);
    check("getauxval(AT_ENTRY) is _start", getauxval(AT_ENTRY) == (unsigned long) _start, 1);
    check("getauxval(AT_UID)", getauxval(AT_UID), 0);
    errno = 0;
    check("getauxval of no type: 0, ENOENT", getauxval(12345) == 0 && errno == ENOENT, 1);

    /* errno values as seen through libc */
    errno = 0;
    check("open missing -> ENOENT", open("/nonexistent", O_RDONLY) == -1 ? errno : 0, ENOENT);
    errno = 0;
    check("rmdir read-only fs -> EROFS", rmdir("/tmp") == -1 ? errno : 0, EROFS);
    errno = 0;
    check("close bad fd -> EBADF", close(99) == -1 ? errno : 0, EBADF);

    /* signal numbers: the kernel must agree on SIGUSR1 (30 in newlib) */
    signal(SIGUSR1, handler);
    check("kill(self, SIGUSR1)", kill(getpid(), SIGUSR1), 0);
    check("SIGUSR1 handler ran", got_signal, SIGUSR1);

    errno = 0;
    check("kill bad signal -> EINVAL", kill(getpid(), 99) == -1 ? errno : 0, EINVAL);

    /* a writable /tmp, for the rest */
    mount("tmpfs", "/tmp", 0, NULL);

    /* scandir() and alphasort() */
    check("scandir: filtered and sorted", scandir_sorted(), 1);
    errno = 0;
    {
        struct dirent **none;
        check("scandir of no directory: ENOENT", scandir("/nonexistent", &none, NULL, NULL) == -1 ? errno : 0, ENOENT);
    }

    /* struct stat layout and times */
    FILE *f = fopen("/tmp/file", "w");
    check("fopen for writing", f != NULL, 1);
    fputs("first line\nsecond line\n", f);
    fclose(f);

    struct stat st;
    check("stat", stat("/tmp/file", &st), 0);
    check("st_size", st.st_size, 23);
    check("S_ISREG", S_ISREG(st.st_mode), 1);
    check("st_mtime is recent", st.st_mtime > 1500000000, 1);

    /* stdio reading and getline */
    f = fopen("/tmp/file", "r");
    char *line = NULL;
    size_t cap = 0;
    check("getline", getline(&line, &cap, f), 11);
    check("getline contents", strcmp(line, "first line\n"), 0);
    free(line);
    fclose(f);

    /* mkdtemp: a directory of its own (newlib makes it only with
     * HAVE_MKDIR, ENOSYS without) */
    char tmpl[] = "/tmp/dir.XXXXXX";
    char *made = mkdtemp(tmpl);
    check("mkdtemp", made != NULL && !strncmp(made, "/tmp/dir.", 9) && strcmp(made + 9, "XXXXXX"), 1);
    check("mkdtemp: a directory", made && !stat(made, &st) && S_ISDIR(st.st_mode) &&
          (st.st_mode & 0777) == 0700, 1);
    if (made)
        rmdir(made);

    /* directories */
    DIR *dir = opendir("/tmp");
    int found = 0;
    struct dirent *ent;
    while (dir && (ent = readdir(dir)))
        if (!strcmp(ent->d_name, "file"))
            found = 1;
    if (dir)
        closedir(dir);
    check("readdir finds the file", found, 1);

    /* pipes, fork and wait */
    int p[2];
    check("pipe", pipe(p), 0);

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        write(p[1], "hello", 5);
        _exit(7);
    }

    close(p[1]);
    char buf[16] = {0};
    ssize_t n, total = 0;
    while ((n = read(p[0], buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    close(p[0]);

    int status;
    check("waitpid", waitpid(pid, &status, 0), pid);
    check("child exit status", WEXITSTATUS(status), 7);
    check("pipe data then EOF", total, 5);
    check("pipe contents", strcmp(buf, "hello"), 0);

    /* misc */
    struct utsname uts;
    check("uname", uname(&uts), 0);
    check("sysname", strcmp(uts.sysname, "AquilaOS"), 0);
    check("time", time(NULL) > 1500000000, 1);

    /* crypt: vectors from the SHA-512 crypt specification */
    const char *h = crypt("Hello world!", "$6$saltstring");
    check("crypt default rounds", h && !strcmp(h, "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/"
        "O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"), 1);
    h = crypt("the minimum number is still observed", "$6$rounds=10$roundstoolow");
    check("crypt minimum rounds", h && !strcmp(h, "$6$rounds=1000$roundstoolow$kUMsbe306n21p9R.FRkW3IGn."
        "S9NPN0x50YhH1xhLsPuWGsUSklZt58jaTfF4ZEQpyUNGc0dqbpBYYBaHHrsX."), 1);
    errno = 0;
    check("crypt unsupported -> EINVAL", crypt("x", "$1$salt") == NULL ? errno : 0, EINVAL);

    /* passwd and shadow (etc/ comes from tests/libc/rootfs) */
    struct passwd *pw = getpwnam("tester");
    check("getpwnam", pw != NULL, 1);
    check("pw_uid", pw ? (long) pw->pw_uid : -1, 1000);

    struct spwd *sp = getspnam("tester");
    check("getspnam", sp != NULL, 1);
    check("sp_lstchg", sp ? sp->sp_lstchg : -2, 19000);
    check("sp_inact empty", sp ? sp->sp_inact : -2, -1);
    check("password verifies", sp && !strcmp(crypt("tester-password", sp->sp_pwdp), sp->sp_pwdp), 1);
    check("wrong password fails", sp && strcmp(crypt("tester-passwore", sp->sp_pwdp), sp->sp_pwdp) != 0, 1);
    check("getspnam unknown", getspnam("nobody") == NULL, 1);

    /* dropping privileges */
    pid = fork();
    if (pid == 0)
        _exit(setgid(100) || setuid(1000) || getuid() != 1000 || setuid(0) == 0 ||
              getspnam("tester") != NULL);
    check("privilege drop", waitpid(pid, &status, 0) == pid && WEXITSTATUS(status) == 0, 1);

    /* truncate (tmpfs mounted above) */
    f = fopen("/tmp/trunc", "w");
    fputs("0123456789", f);
    fclose(f);
    check("truncate", truncate("/tmp/trunc", 4), 0);
    check("truncated size", stat("/tmp/trunc", &st) == 0 ? (long) st.st_size : -1, 4);
    int tfd = open("/tmp/trunc", O_RDWR);
    check("ftruncate grow", ftruncate(tfd, 8), 0);
    char tbuf[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    check("read grown file", read(tfd, tbuf, 8), 8);
    check("grown part zeroed", memcmp(tbuf, "0123\0\0\0\0", 8), 0);
    close(tfd);

    /* wait3, getppid */
    pid_t self = getpid();
    pid = fork();
    if (pid == 0)
        _exit(getppid() == self ? 3 : 4);
    check("wait3", wait3(&status, 0, NULL), pid);
    check("wait3 status", WEXITSTATUS(status), 3);

    /* killpg: the child makes its own group, then waits to be killed */
    pipe(p);
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        write(p[1], "x", 1);
        for (;;)
            getpid();
    }
    read(p[0], buf, 1);
    close(p[0]);
    close(p[1]);
    check("killpg", killpg(pid, SIGTERM), 0);
    check("killpg wait", waitpid(pid, &status, 0), pid);
    check("killed by SIGTERM", WIFSIGNALED(status) ? WTERMSIG(status) : 0, SIGTERM);

    /* signal mask: a blocked signal stays pending until sigsuspend */
    sigset_t block, old, none;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    sigemptyset(&none);
    signal(SIGUSR2, handler);
    got_signal = 0;
    check("sigprocmask block", sigprocmask(SIG_BLOCK, &block, &old), 0);
    kill(getpid(), SIGUSR2);
    check("blocked signal not delivered", got_signal, 0);
    errno = 0;
    check("sigsuspend -> EINTR", sigsuspend(&none) == -1 ? errno : 0, EINTR);
    check("delivered in sigsuspend", got_signal, SIGUSR2);
    sigprocmask(SIG_SETMASK, &old, NULL);

    /* byte order */
    check("htonl", htonl(0x01020304) == 0x04030201, 1);
    check("ntohs", ntohs(htons(0xabcd)), 0xabcd);

    /* files through the *at() calls and the functions built on them */
    check("mkdir", mkdir("/tmp/dir", 0755), 0);
    int dfd = open("/tmp/dir", O_RDONLY | O_DIRECTORY);
    check("open directory", dfd >= 0, 1);
    tfd = openat(dfd, "f", O_CREAT | O_WRONLY, 0644);
    check("openat", tfd >= 0, 1);
    write(tfd, "data", 4);
    close(tfd);
    check("fstatat", fstatat(dfd, "f", &st, 0) == 0 && st.st_size == 4, 1);
    check("symlink", symlink("dir/f", "/tmp/link"), 0);
    char lbuf[32] = {0};
    check("readlink", readlink("/tmp/link", lbuf, sizeof(lbuf)), 5);
    check("readlink contents", strcmp(lbuf, "dir/f"), 0);
    char *rp = realpath("/tmp/./link", NULL);
    check("realpath through a link", rp && !strcmp(rp, "/tmp/dir/f"), 1);
    free(rp);
    symlink("/tmp/dir", "/tmp/dl");
    check("realpath .. after a link", realpath("/tmp/dl/../dir/f", lbuf) && !strcmp(lbuf, "/tmp/dir/f"), 1);
    chdir("/tmp/dir");
    check("realpath relative", realpath("../dl/./f", lbuf) && !strcmp(lbuf, "/tmp/dir/f"), 1);
    chdir("/");
    errno = 0;
    check("realpath file as directory", realpath("/tmp/dir/f/x", lbuf) == NULL ? errno : 0, ENOTDIR);
    errno = 0;
    check("realpath missing", realpath("/tmp/nothing/x", lbuf) == NULL ? errno : 0, ENOENT);
    check("rename a directory", rename("/tmp/dir", "/tmp/dir2"), 0);
    check("renamed", stat("/tmp/dir2/f", &st), 0);
    DIR *dp = fdopendir(openat(AT_FDCWD, "/tmp/dir2", O_RDONLY | O_DIRECTORY));
    int entries = 0;
    struct dirent *de;
    while (dp && (de = readdir(dp)))
        entries += !strcmp(de->d_name, "f");
    check("fdopendir", entries, 1);
    if (dp)
        closedir(dp);
    errno = 0;
    check("rmdir not empty", rmdir("/tmp/dir2") == -1 ? errno : 0, ENOTEMPTY);
    check("unlinkat", unlinkat(AT_FDCWD, "/tmp/dir2/f", 0), 0);
    check("rmdir", rmdir("/tmp/dir2"), 0);
    close(dfd);
    check("chmod", chmod("/tmp/trunc", 0600) == 0 && stat("/tmp/trunc", &st) == 0 && (st.st_mode & 0777) == 0600, 1);
    struct timespec times[2] = {{1000, 0}, {2000, 0}};
    check("utimensat", utimensat(AT_FDCWD, "/tmp/trunc", times, 0), 0);
    check("mtime set", stat("/tmp/trunc", &st) == 0 && st.st_mtime == 2000, 1);

    /* /etc/group (tests/libc/rootfs) */
    struct group *gr = getgrnam("users");
    check("getgrnam", gr && gr->gr_gid == 100, 1);
    check("group members", gr && gr->gr_mem[0] && !strcmp(gr->gr_mem[1], "other") && !gr->gr_mem[2], 1);
    gr = getgrgid(0);
    check("getgrgid", gr && !strcmp(gr->gr_name, "root"), 1);
    check("getgrnam unknown", getgrnam("nobody") == NULL, 1);

    /* host name */
    char host[64];
    check("sethostname", sethostname("testhost", 8), 0);
    check("gethostname", gethostname(host, sizeof(host)) == 0 && !strcmp(host, "testhost"), 1);

    /* sleeping and clocks */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    check("usleep", usleep(100000), 0);
    check("sleep", sleep(1), 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check("slept 1.1s", ms >= 1100 && ms < 2000, 1);

    /* terminals: the serial port is one, a pipe isn't */
    check("isatty tty", isatty(fileno(out)), 1);
    char *tty = ttyname(fileno(out));
    check("ttyname", tty && !strcmp(tty, "/dev/ttyS0"), 1);
    pipe(p);
    errno = 0;
    check("isatty pipe", isatty(p[0]) == 0 && errno == ENOTTY, 1);
    errno = 0;
    check("lseek pipe -> ESPIPE", lseek(p[0], 0, SEEK_CUR) == -1 ? errno : 0, ESPIPE);

    /* writing to a pipe without readers */
    close(p[0]);
    signal(SIGPIPE, SIG_IGN);
    errno = 0;
    check("EPIPE", write(p[1], "x", 1) == -1 ? errno : 0, EPIPE);
    signal(SIGPIPE, SIG_DFL);
    pid = fork();
    if (pid == 0) {
        write(p[1], "x", 1);
        _exit(0);
    }
    close(p[1]);
    check("SIGPIPE", waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) ? WTERMSIG(status) : 0, SIGPIPE);

    /* select and poll through libc */
    int sp2[2];
    pipe(sp2);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sp2[0], &rfds);
    struct timeval tv = {0, 100000};
    check("select timeout", select(sp2[0] + 1, &rfds, NULL, NULL, &tv), 0);
    write(sp2[1], "s", 1);
    FD_ZERO(&rfds);
    FD_SET(sp2[0], &rfds);
    check("select readable", select(sp2[0] + 1, &rfds, NULL, NULL, NULL), 1);
    check("FD_ISSET", FD_ISSET(sp2[0], &rfds) != 0, 1);
    struct pollfd pfd = {sp2[0], POLLIN, 0};
    check("poll", poll(&pfd, 1, -1) == 1 && pfd.revents == POLLIN, 1);
    close(sp2[0]);
    close(sp2[1]);

    /* siginfo and sigsetjmp: a fault reports its address */
    struct sigaction fsa = {0};
    fsa.sa_sigaction = fault_handler;
    fsa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &fsa, NULL);
    int jumped = sigsetjmp(fault_jmp, 1);
    if (!jumped)
        *(volatile int *) 0x1234 = 1;
    check("SIGSEGV handler ran", jumped, SIGSEGV);
    check("si_addr", (long) fault_addr, 0x1234);
    signal(SIGSEGV, SIG_DFL);

    /* pselect: the signal blocked outside is let through while waiting */
    sigset_t blk, empty, cur;
    sigemptyset(&blk);
    sigaddset(&blk, SIGUSR1);
    sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    signal(SIGUSR1, handler);
    got_signal = 0;
    pipe(sp2);
    pid = fork();
    if (pid == 0) {
        usleep(100000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }
    FD_ZERO(&rfds);
    FD_SET(sp2[0], &rfds);
    struct timespec pts = {2, 0};
    errno = 0;
    check("pselect -> EINTR", pselect(sp2[0] + 1, &rfds, NULL, NULL, &pts, &empty) == -1 ? errno : 0, EINTR);
    check("signal delivered in pselect", got_signal, SIGUSR1);
    sigprocmask(SIG_SETMASK, NULL, &cur);
    check("mask restored", sigismember(&cur, SIGUSR1), 1);
    sigprocmask(SIG_UNBLOCK, &blk, NULL);
    waitpid(pid, &status, 0);
    close(sp2[0]);
    close(sp2[1]);

    /* descriptors */
    tfd = open("/tmp/trunc", O_RDONLY);
    errno = 0;
    check("write read-only -> EBADF", write(tfd, "x", 1) == -1 ? errno : 0, EBADF);
    check("fsync", fsync(tfd), 0);
    close(tfd);
    errno = 0;
    check("fsync closed -> EBADF", fsync(tfd) == -1 ? errno : 0, EBADF);

    /* the serial port is init's controlling terminal */
    check("tcgetsid", tcgetsid(fileno(out)), getpid());
    check("tcflush", tcflush(fileno(out), TCIFLUSH), 0);

    /* wcwidth: control characters aren't printable (-1, not 0, which
     * programs take for a combining character) */
    check("wcwidth newline", wcwidth(L'\n'), -1);
    check("wcwidth NUL", wcwidth(L'\0'), 0);
    check("wcwidth letter", wcwidth(L'a'), 1);

    /* C99 printf formats */
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%zu %jd %hhu", (size_t) 42, (intmax_t) -7, (unsigned char) 300);
    check("printf %zu %jd %hhu", strcmp(fmt, "42 -7 44"), 0);

    /* flock: EWOULDBLOCK for a lock another open file holds */
    int lk = open("/tmp/flock", O_CREAT | O_RDWR, 0644);
    int lk2 = open("/tmp/flock", O_RDWR);
    check("flock", flock(lk, LOCK_EX), 0);
    errno = 0;
    check("flock held -> EWOULDBLOCK", flock(lk2, LOCK_SH | LOCK_NB) == -1 ? errno : 0, EWOULDBLOCK);
    close(lk);
    check("flock after close", flock(lk2, LOCK_SH | LOCK_NB), 0);
    close(lk2);

    /* reboot: only root */
    check("reboot enable Ctrl+Alt+Del", reboot(RB_ENABLE_CAD), 0);
    pid_t rb = fork();
    if (!rb) {
        setuid(1000);
        _exit(reboot(RB_AUTOBOOT) == -1 && errno == EPERM ? 0 : 1);
    }
    int rb_status;
    waitpid(rb, &rb_status, 0);
    check("reboot unprivileged -> EPERM", WEXITSTATUS(rb_status), 0);

    test_x_support();
    test_pthread();
    test_pthread_signals();
    test_siginfo();
    test_posix_calls();
    test_tcp();
    test_err();
    test_resolver();

    /* virtual terminals */
    test_vt();

    fprintf(out, "aqtest: done, fails=%d\n", fails);

    for (;;)
        getpid();
}
