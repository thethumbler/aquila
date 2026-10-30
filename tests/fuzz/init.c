/*
 * System calls with bad arguments: the kernel must survive them all
 *
 * init (this program) forks workers one after another, for fuzz.secs
 * seconds. Each worker makes fuzz.calls system calls chosen at random,
 * with arguments that are mostly wrong but plausible: descriptors that
 * are open, closed or out of range; pointers to its memory, to nothing,
 * to the kernel, to read-only memory, running into an unmapped page or
 * past the end of user space; lengths of 0, a page, or huge; paths that
 * exist, don't, are too long or have no end; structures filled with
 * such values. Half the workers run as root, half as user 1000; some
 * run the same in two more threads, sharing their descriptors and
 * memory.
 *
 * A worker may end however it likes: exiting, killed by a signal its
 * own calls brought, or by init after a time limit if a call waits. The
 * test fails if the kernel panics or resets (tests/run.sh sees it), if a
 * worker can't be killed, or if init can't fork after.
 *
 * Each worker's random numbers come from its seed, printed before it
 * runs ("fuzz: seed N"), so the last seed before a panic replays it:
 * the kernel's command line (tests/run.sh -k) takes
 *
 *      fuzz.seed=N     the first worker's seed (default 1)
 *      fuzz.workers=N  workers to run (default: as many as fit the time)
 *      fuzz.secs=N     how long to run (default 30)
 *      fuzz.calls=N    calls per worker (default 2000)
 *      fuzz.trace      print each call before it's made
 *
 * The calls depend on the system's state too (process ids, time), so a
 * replay is close to the run, not always the same.
 */

#include "../lib/aqtest.h"

#include <aquila/fb.h>
#include <aquila/in.h>
#include <aquila/kd.h>
#include <aquila/mman.h>
#include <aquila/socket.h>
#include <aquila/un.h>
#include <aquila/vt.h>
#include <aquila/wait.h>

#if defined(__x86_64__) || defined(__aarch64__)
#define ULIMIT      0x0000800000000000ul    /* where user space ends */
#define NONCANON    0x8000000000000000ul
#else
#define ULIMIT      0xC0000000ul
#define NONCANON    0xFFFFFFF0ul
#endif

#define FD_TRACE    60          /* where a tracing worker keeps the serial port */
#define NFDS        16

static unsigned long first_seed = 1, max_workers = 0, secs = 30, ncalls = 2000;
static int trace;

/*
 * Random numbers: xorshift64*, one state per thread
 */

struct rng {
    uint64_t s;
};

static uint64_t rnd(struct rng *r)
{
    r->s ^= r->s >> 12;
    r->s ^= r->s << 25;
    r->s ^= r->s >> 27;
    return r->s * 0x2545F4914F6CDD1Dull;
}

static unsigned rndn(struct rng *r, unsigned n)
{
    return (unsigned) (rnd(r) >> 33) % n;
}

static void rng_seed(struct rng *r, uint64_t seed)
{
    r->s = seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    rnd(r);
}

/*
 * What the arguments point to
 */

/* scratch memory, mapped by itself so that what the kernel writes into
 * it reaches nothing of the fuzzer's: the structures' fields, a socket
 * address, a buffer, then a page with nothing mapped after it (guard) */
#define NWORDS      512
#define BUF_SIZE    65536
#define SCRATCH     (NWORDS * sizeof(uptr) + 4096 + BUF_SIZE)

static uptr *words;             /* a structure's worth of random values */
static char *sockaddr;          /* 4096 bytes */
static char *buf;               /* BUF_SIZE bytes */
static char *guard;             /* buf's last page: none after it */
static char longpath[8192];
static int fds[NFDS];
static int nfds;
static int child;               /* the last child forked */

static const char *paths[] = {
    "/tmp/fz/f", "/tmp/fz/d", "/tmp/fz/d/x", "/tmp/fz/l", "/tmp/fz/n",
    "/tmp/fz/d/y", "/tmp/fz/l/x", "/tmp/fz/f/", "/tmp/fz", "/tmp", "/",
    "f", "d", "l", "n", "d/x", ".", "..", "", "///", "d/../d/./x",
    "/dev", "/dev/null", "/dev/zero", "/dev/full", "/dev/urandom",
    "/dev/ptmx", "/dev/fb0", "/dev/tty1", "/dev/vcs1", "/dev/kmsg",
    "/proc", "/proc/meminfo", "/proc/1/status", "/proc/interrupts",
    "/no/such/file",
};

#define NPATHS  (sizeof(paths) / sizeof(paths[0]))

/* the files a worker may make: removed before the next one starts */
static const char *made[] = {
    "/tmp/fz/d/x/y", "/tmp/fz/d/x", "/tmp/fz/d/y", "/tmp/fz/n",
    "/tmp/fz/f", "/tmp/fz/l", "/tmp/fz/d", "/tmp/fz/d/n",
};

static const unsigned long ioctls[] = {
    TCGETS, TCSETS, TCSETSW, TCSETSF, TCSBRK, TCXONC, TCFLSH, TIOCSCTTY,
    TIOCGPGRP, TIOCSPGRP, TIOCGWINSZ, TIOCSWINSZ, TIOCGSID, TIOCGPTN,
    TIOCSPTLCK, FIONREAD, FIONBIO, TIOCNOTTY, TIOCSTI,
    FBIOGET_FSCREENINFO, FBIOGET_VSCREENINFO, FBIOPAN_DISPLAY,
    FBIOPUT_VSCREENINFO, KDGETMODE, KDSETMODE, KDGKBMODE, KDSKBMODE,
    KDGKBENT, KDGKBTYPE, KDMKTONE, KDSETLED, VT_ACTIVATE, VT_DISALLOCATE,
    VT_GETMODE, VT_SETMODE, VT_GETSTATE, VT_OPENQRY, VT_RELDISP,
    VT_WAITACTIVE, SIOCADDRT, SIOCDELRT, SIOCGIFADDR, SIOCSIFADDR,
    SIOCGIFBRDADDR, SIOCGIFCONF, SIOCGIFFLAGS, SIOCSIFFLAGS, SIOCGIFHWADDR,
    SIOCGIFINDEX, SIOCGIFMTU, SIOCGIFNETMASK, SIOCSIFNETMASK,
};

static const uptr devs[] = {
    DEV(1, 3), DEV(1, 5), DEV(1, 7), DEV(1, 8), DEV(1, 9), DEV(1, 11),
    DEV(4, 1), DEV(5, 0), DEV(5, 2), DEV(7, 1), DEV(7, 129), DEV(29, 0),
    DEV(10, 1), DEV(3, 0), DEV(200, 0), 0,
};

/* devices that root may break the machine with on purpose: /dev/mem,
 * /dev/kmem, /dev/port, /dev/core */
static int dangerous_dev(uptr dev)
{
    dev &= 0xFFFF;
    return (dev >> 8) == 1 && ((dev & 0xFF) == 1 || (dev & 0xFF) == 2 ||
                               (dev & 0xFF) == 4 || (dev & 0xFF) == 6);
}

/*
 * Arguments, by kind
 */

enum {
    A_ANY, A_INT, A_LEN, A_FD, A_PTR, A_PATH, A_STRUCT, A_ADDR, A_MODE,
    A_FLAGS, A_PID, A_SIG, A_DEV, A_IOCTL, A_NKINDS,
};

static uptr arg(struct rng *r, int kind);

static uptr a_int(struct rng *r)
{
    switch (rndn(r, 10)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (uptr) -1;
    case 3: return rndn(r, 8);
    case 4: return rndn(r, 64);
    case 5: return (uptr) 1 << rndn(r, 8 * sizeof(uptr));
    case 6: return 0x7FFFFFFF;
    case 7: return 0x80000000u;
    case 8: return (uptr) -(long) rndn(r, 4096);
    default: return (uptr) rnd(r);
    }
}

static uptr a_len(struct rng *r)
{
    static const uptr lens[] = {
        0, 1, 2, 7, 8, 16, 64, 255, 256, 4095, 4096, 4097, 65536, 1 << 20,
        0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, (uptr) -1, (uptr) -4096,
    };

    if (rndn(r, 4) == 0)
        return rndn(r, 8192);
    return lens[rndn(r, sizeof(lens) / sizeof(lens[0]))];
}

static uptr a_fd(struct rng *r)
{
    switch (rndn(r, 8)) {
    case 0: return (uptr) -1;
    case 1: return rndn(r, 70);
    case 2: return 0x7FFFFFFF;
    default: return nfds ? (uptr) fds[rndn(r, nfds)] : 0;
    }
}

static uptr a_ptr(struct rng *r)
{
    switch (rndn(r, 14)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return KADDR;
    case 3: return (uptr) guard + 4096 - 1 - rndn(r, 16);  /* into the hole */
    case 4: return (uptr) guard + 4096;                     /* the hole */
    case 5: return ULIMIT - 1 - rndn(r, 16);               /* past user space */
    case 6: return ULIMIT;
    case 7: return NONCANON;
    case 8: return (uptr) aq_main;                          /* read-only */
    case 9: return (uptr) -1;
    case 10: return (uptr) rnd(r);
    default: return (uptr) &buf[rndn(r, BUF_SIZE)];
    }
}

static uptr a_path(struct rng *r)
{
    switch (rndn(r, 12)) {
    case 0: return (uptr) longpath + rndn(r, sizeof(longpath) - 1);
    case 1: {
        /* no NUL before the hole */
        char *p = guard + 4096 - 1 - rndn(r, 64);
        for (char *q = p; q < guard + 4096; ++q)
            *q = 'a';
        return (uptr) p;
    }
    case 2: return a_ptr(r);
    default: return (uptr) paths[rndn(r, NPATHS)];
    }
}

/* a structure, its fields random values of every kind */
static uptr a_struct(struct rng *r)
{
    int n = 8 + rndn(r, 24);
    uptr *w = &words[rndn(r, 8)];

    for (int i = 0; i < n; ++i) {
        int kind = rndn(r, 3) ? A_INT : rndn(r, A_NKINDS);
        w[i] = kind == A_STRUCT ? (uptr) words : arg(r, kind);
    }

    return rndn(r, 8) ? (uptr) w : a_ptr(r);
}

static uptr a_addr(struct rng *r)
{
    char *s = sockaddr;

    for (unsigned i = 0; i < 256; ++i)
        s[i] = rndn(r, 4) ? 0 : (char) rnd(r);

    switch (rndn(r, 4)) {
    case 0: {
        struct sockaddr_in *in = (struct sockaddr_in *) s;
        static const uint32_t addrs[] = {
            0x0100007F, 0, 0xFFFFFFFF, 0x0202000A, 0x0F02000A, 0x0002000A,
        };
        in->sin_family = AF_INET;
        in->sin_port = rndn(r, 2) ? (uint16_t) rnd(r) : 0x3930;     /* 12345 */
        in->sin_addr.s_addr = rndn(r, 4) ? addrs[rndn(r, 6)] : (uint32_t) rnd(r);
        break;
    }
    case 1: {
        struct sockaddr_un *un = (struct sockaddr_un *) s;
        un->sun_family = AF_UNIX;
        const char *p = paths[rndn(r, NPATHS)];
        int i = 0;
        for (; p[i] && i < 127; ++i)
            un->sun_path[i] = p[i];
        if (rndn(r, 4) == 0)        /* no end */
            for (; i < 128; ++i)
                un->sun_path[i] = 'u';
        break;
    }
    case 2:
        *(unsigned short *) s = (unsigned short) rndn(r, 8);
        break;
    default:
        return a_ptr(r);
    }

    return (uptr) s;
}

static uptr a_mode(struct rng *r)
{
    static const uptr types[] = {
        0, S_IFREG, S_IFDIR, S_IFCHR, S_IFBLK, S_IFIFO, S_IFSOCK, S_IFLNK,
    };

    if (rndn(r, 6) == 0)
        return a_int(r);
    return types[rndn(r, 8)] | (rndn(r, 2) ? 0644 : rndn(r, 07777 + 1));
}

static uptr a_flags(struct rng *r)
{
    static const uptr flags[] = {
        O_WRONLY, O_RDWR, O_APPEND, O_CREAT, O_TRUNC, O_EXCL, O_NONBLOCK,
        O_NOCTTY, O_CLOEXEC, O_NOFOLLOW, O_DIRECTORY, O_EXEC, O_SYNC,
    };
    uptr f = 0;

    for (int n = rndn(r, 4); n; --n)
        f |= flags[rndn(r, sizeof(flags) / sizeof(flags[0]))];

    return rndn(r, 8) ? f : a_int(r);
}

static uptr a_pid(struct rng *r)
{
    switch (rndn(r, 7)) {
    case 0: return 0;
    case 1: return SC(SYS_getpid, 0, 0, 0);
    case 2: return -SC(SYS_getpid, 0, 0, 0);
    case 3: return child;
    case 4: return 99999;
    case 5: return (uptr) -1;
    default: return 2 + rndn(r, 64);
    }
}

static uptr arg(struct rng *r, int kind)
{
    if (kind != A_ANY && rndn(r, 8) == 0)
        kind = A_ANY;

    if (kind == A_ANY)
        kind = 1 + rndn(r, A_NKINDS - 1);

    switch (kind) {
    case A_INT:     return a_int(r);
    case A_LEN:     return a_len(r);
    case A_FD:      return a_fd(r);
    case A_PTR:     return a_ptr(r);
    case A_PATH:    return a_path(r);
    case A_STRUCT:  return a_struct(r);
    case A_ADDR:    return a_addr(r);
    case A_MODE:    return a_mode(r);
    case A_FLAGS:   return a_flags(r);
    case A_PID:     return a_pid(r);
    case A_SIG:     return rndn(r, 8) ? rndn(r, 34) : a_int(r);
    case A_DEV:     return rndn(r, 8) ? devs[rndn(r, sizeof(devs) / sizeof(devs[0]))] : a_int(r);
    case A_IOCTL:   return ioctls[rndn(r, sizeof(ioctls) / sizeof(ioctls[0]))];
    }

    return 0;
}

/*
 * The calls: each one's arguments, by kind
 */

struct call {
    int nr;
    unsigned char args[5];
};

static const struct call calls[] = {
    {SYS_close,         {A_FD}},
    {SYS_execve,        {A_PATH, A_STRUCT, A_STRUCT}},
    {SYS_fork},
    {SYS_fstat,         {A_FD, A_PTR}},
    {SYS_getpid},
    {SYS_isatty,        {A_FD}},
    {SYS_kill,          {A_PID, A_SIG}},
    {SYS_link,          {A_PATH, A_PATH}},
    {SYS_lseek,         {A_FD, A_ANY, A_INT}},
    {SYS_open,          {A_PATH, A_FLAGS, A_MODE}},
    {SYS_read,          {A_FD, A_PTR, A_LEN}},
    {SYS_sbrk,          {A_ANY}},
    {SYS_stat,          {A_PATH, A_PTR}},
    {SYS_times,         {A_PTR}},
    {SYS_unlink,        {A_PATH}},
    {SYS_waitpid,       {A_PID, A_PTR, A_INT}},
    {SYS_write,         {A_FD, A_PTR, A_LEN}},
    {SYS_ioctl,         {A_FD, A_IOCTL, A_STRUCT}},
    {SYS_ioctl,         {A_FD, A_INT, A_PTR}},
    {SYS_sigaction,     {A_SIG, A_STRUCT, A_PTR}},
    {SYS_readdir,       {A_FD, A_PTR}},
    {SYS_mount,         {A_STRUCT}},
    {SYS_mkdir,         {A_PATH, A_MODE}},
    {SYS_uname,         {A_PTR}},
    {SYS_pipe,          {A_PTR}},
    {SYS_fcntl,         {A_FD, A_INT, A_ANY}},
    {SYS_chdir,         {A_PATH}},
    {SYS_getcwd,        {A_PTR, A_LEN}},
    {SYS_thread_create, {A_STRUCT}},
    {SYS_thread_exit,   {A_ANY}},
    {SYS_thread_join,   {A_INT, A_PTR}},
    {SYS_setpgid,       {A_PID, A_PID}},
    {SYS_mknod,         {A_PATH, A_MODE, A_DEV}},
    {SYS_lstat,         {A_PATH, A_PTR}},
    {SYS_getuid},
    {SYS_getgid},
    {SYS_mmap,          {A_STRUCT, A_PTR}},
    {SYS_munmap,        {A_PTR, A_LEN}},
    {SYS_socket,        {A_INT, A_INT, A_INT}},
    {SYS_accept,        {A_FD, A_ADDR, A_PTR}},
    {SYS_bind,          {A_FD, A_ADDR, A_LEN}},
    {SYS_connect,       {A_FD, A_ADDR, A_LEN}},
    {SYS_listen,        {A_FD, A_INT}},
    {SYS_send,          {A_STRUCT}},
    {SYS_recv,          {A_STRUCT}},
    {SYS_umask,         {A_MODE}},
    {SYS_chmod,         {A_PATH, A_MODE}},
    {SYS_sysconf,       {A_INT}},
    {SYS_access,        {A_PATH, A_INT}},
    {SYS_gettimeofday,  {A_PTR, A_PTR}},
    {SYS_sigmask,       {A_INT, A_STRUCT, A_PTR}},
    {SYS_select,        {A_STRUCT}},
    {SYS_getpgrp},
    {SYS_chown,         {A_PATH, A_INT, A_INT}},
    {SYS_fchown,        {A_FD, A_INT, A_INT}},
    {SYS_lchown,        {A_PATH, A_INT, A_INT}},
    {SYS_rmdir,         {A_PATH}},
    {SYS_setuid,        {A_INT}},
    {SYS_setgid,        {A_INT}},
    {SYS_ftruncate,     {A_FD, A_ANY}},
    {SYS_sigsuspend,    {A_STRUCT}},
    {SYS_getppid},
    {SYS_openat,        {A_FD, A_PATH, A_FLAGS, A_MODE}},
    {SYS_fstatat,       {A_FD, A_PATH, A_PTR, A_INT}},
    {SYS_unlinkat,      {A_FD, A_PATH, A_INT}},
    {SYS_mknodat,       {A_FD, A_PATH, A_MODE, A_DEV}},
    {SYS_linkat,        {A_FD, A_PATH, A_FD, A_PATH, A_INT}},
    {SYS_symlinkat,     {A_PATH, A_FD, A_PATH}},
    {SYS_readlinkat,    {A_FD, A_PATH, A_PTR, A_LEN}},
    {SYS_renameat,      {A_FD, A_PATH, A_FD, A_PATH}},
    {SYS_faccessat,     {A_FD, A_PATH, A_INT, A_INT}},
    {SYS_fchmodat,      {A_FD, A_PATH, A_MODE, A_INT}},
    {SYS_fchownat,      {A_FD, A_PATH, A_INT, A_INT, A_INT}},
    {SYS_utimensat,     {A_FD, A_PATH, A_STRUCT, A_INT}},
    {SYS_fchmod,        {A_FD, A_MODE}},
    {SYS_fchdir,        {A_FD}},
    {SYS_setsid},
    {SYS_nanosleep,     {A_STRUCT, A_PTR}},
    {SYS_clock_gettime, {A_INT, A_PTR}},
    {SYS_sethostname,   {A_PTR, A_LEN}},
    {SYS_poll,          {A_STRUCT, A_LEN, A_INT}},
    {SYS_flock,         {A_FD, A_INT}},
    {SYS_sync},
    {SYS_shutdown,      {A_FD, A_INT}},
    {SYS_setitimer,     {A_INT, A_STRUCT, A_PTR}},
    {SYS_thread_self},
    {SYS_getrandom,     {A_PTR, A_LEN, A_INT}},
    {SYS_getsockname,   {A_FD, A_ADDR, A_PTR}},
    {SYS_getpeername,   {A_FD, A_ADDR, A_PTR}},
    {SYS_getsockopt,    {A_FD, A_INT, A_INT, A_PTR, A_PTR}},
    {SYS_setsockopt,    {A_FD, A_INT, A_INT, A_PTR, A_LEN}},
    {SYS_sigreturn},
    {0,                 {A_ANY, A_ANY, A_ANY, A_ANY, A_ANY}},   /* any number */
};

#define NCALLS  (sizeof(calls) / sizeof(calls[0]))

/*
 * Printing, for the trace
 */

static void put(int fd, const char *s)
{
    SC(SYS_write, fd, s, aq_strlen(s));
}

static void puthex(int fd, uptr v)
{
    char b[2 + 2 * sizeof(uptr) + 1];
    int i = sizeof(b) - 1;

    b[i] = 0;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    b[--i] = 'x';
    b[--i] = '0';
    put(fd, &b[i]);
}

static void putdec(int fd, unsigned long v)
{
    char b[24];
    int i = sizeof(b) - 1;

    b[i] = 0;
    do { b[--i] = '0' + v % 10; v /= 10; } while (v);
    put(fd, &b[i]);
}

/*
 * A worker
 */

#define SIGALRM_BITS    (3ul << (SIGALRM - 1))  /* the bit, counted from 0 or from 1 */

static void on_alarm(int sig)
{
}

/* SIGALRM every 2 ms, to a handler that does nothing: a call that waits
 * (sigsuspend, a read, nanosleep, ...) is interrupted, the worker goes on */
static void alarm_arm(void)
{
    struct aquila_sigaction sa = {(unsigned long) on_alarm, 0, 0};
    SC(SYS_sigaction, SIGALRM, &sa, 0);

    /* a write to a closed pipe or socket fails (EPIPE) */
    struct aquila_sigaction ign = {1, 0, 0};       /* SIG_IGN */
    SC(SYS_sigaction, SIGPIPE, &ign, 0);

    unsigned long unblock = SIGALRM_BITS;
    SC(SYS_sigmask, SIG_UNBLOCK, &unblock, 0);

    struct aquila_itimer it = {2000000, 2000000};
    SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
}

static void fuzz(struct rng *r, unsigned long n)
{
    for (unsigned long i = 0; i < n; ++i) {
        const struct call *c = &calls[rndn(r, NCALLS)];
        long nr = c->nr;
        uptr a[5];

        /* rarely, the calls that usually end the worker (a handler at a
         * random address the next signal jumps to, a sigreturn without a
         * frame, a thread starting at a random address, its memory
         * unmapped) or wait for good: the calls after them are what's
         * tested */
        if ((nr == SYS_sigreturn || nr == SYS_sigaction || nr == SYS_sigsuspend ||
             nr == SYS_thread_create || nr == SYS_thread_exit || nr == SYS_kill ||
             nr == SYS_munmap || nr == SYS_sbrk) && rndn(r, 32))
            continue;

        if (!nr) {
            static const long odd[] = {0, 35, 58, SYS_MAX + 1, 1000, -1, 0x80000000u};
            nr = rndn(r, 2) ? odd[rndn(r, 7)] : (long) rnd(r);
        }

        for (int j = 0; j < 5; ++j)
            a[j] = c->args[j] || !c->nr ? arg(r, c->args[j]) : arg(r, A_INT);

        /* what would end the test, not find a bug: init killed, the
         * machine rebooted, root writing the kernel's memory */
        if (nr == SYS_reboot || nr == SYS_exit)
            continue;
        if (nr == SYS_kill && (a[0] == 1 || a[0] == (uptr) -1))
            continue;
        if ((nr == SYS_mknod && dangerous_dev(a[2])) ||
            (nr == SYS_mknodat && dangerous_dev(a[3])))
            continue;

        if (trace) {
            put(FD_TRACE, "fuzz: ");
            putdec(FD_TRACE, i);
            put(FD_TRACE, ": ");
            putdec(FD_TRACE, nr);
            for (int j = 0; j < 5; ++j) {
                put(FD_TRACE, j ? ", " : " (");
                puthex(FD_TRACE, a[j]);
            }
            put(FD_TRACE, ")\n");
        }

        /* the alarm stays deliverable */
        if (nr == SYS_sigsuspend && a[0] >= (uptr) words && a[0] < (uptr) (words + NWORDS))
            *(uptr *) a[0] &= ~SIGALRM_BITS;

        long ret = SC5(nr, a[0], a[1], a[2], a[3], a[4]);

        /* and is put back if the call took it away */
        if (nr == SYS_sigaction || nr == SYS_sigmask || nr == SYS_setitimer)
            alarm_arm();

        if (nr == SYS_fork) {
            if (ret == 0)
                SC(SYS_exit, 0, 0, 0);
            if (ret > 0)
                child = ret;
        }

        /* a new descriptor joins the ones to use */
        if ((nr == SYS_open || nr == SYS_openat || nr == SYS_socket ||
             nr == SYS_accept) && ret >= 0 && ret < 64 && nfds < NFDS)
            fds[nfds++] = ret;
    }
}

#define STACK_SIZE  16384

static char stacks[2][STACK_SIZE] __attribute__((aligned(16)));
static uint64_t thread_seeds[2];

static void __attribute__((noreturn)) fuzz_thread(int t)
{
    struct rng r;
    rng_seed(&r, thread_seeds[t]);
    fuzz(&r, ncalls);
    SC(SYS_thread_exit, 0, 0, 0);
    for (;;);
}

/* a thread starts with no arguments in registers: one function each */
static void fuzz_thread0(void) { fuzz_thread(0); }
static void fuzz_thread1(void) { fuzz_thread(1); }

/* the scratch memory, and the page after it unmapped */
static void scratch_map(void)
{
    struct aquila_mmap_args m = {0, SCRATCH + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
    char *p = 0;

    if (SC(SYS_mmap, &m, &p, 0) || !p)
        SC(SYS_exit, 1, 0, 0);

    SC(SYS_munmap, p + SCRATCH, 4096, 0);

    words = (uptr *) p;
    sockaddr = p + NWORDS * sizeof(uptr);
    buf = sockaddr + 4096;
    guard = buf + BUF_SIZE - 4096;
}

static void open_fd(long fd)
{
    if (fd >= 0 && nfds < NFDS)
        fds[nfds++] = fd;
}

/* the files and descriptors to start from */
static void setup(void)
{
    for (unsigned i = 0; i < sizeof(made) / sizeof(made[0]); ++i) {
        SC(SYS_unlink, made[i], 0, 0);
        SC(SYS_rmdir, made[i], 0, 0);
    }

    SC(SYS_mkdir, "/tmp/fz", 0777, 0);
    SC(SYS_mkdir, "/tmp/fz/d", 0777, 0);
    SC(SYS_chdir, "/tmp/fz", 0, 0);

    int fd = SC(SYS_open, "/tmp/fz/f", O_RDWR | O_CREAT, 0666);
    SC(SYS_write, fd, buf, 5000);
    open_fd(fd);
    SC(SYS_symlinkat, "f", -100, "/tmp/fz/l");

    open_fd(SC(SYS_open, "/tmp/fz/d", O_RDONLY, 0));
    open_fd(SC(SYS_open, "/dev/null", O_RDWR, 0));
    open_fd(SC(SYS_open, "/dev/zero", O_RDWR, 0));
    open_fd(SC(SYS_open, "/dev/ptmx", O_RDWR | O_NONBLOCK, 0));
    open_fd(SC(SYS_open, "/proc/meminfo", O_RDONLY, 0));

    int p[2];
    if (SC(SYS_pipe, p, 0, 0) == 0) {
        SC(SYS_fcntl, p[0], F_SETFL, O_NONBLOCK);
        SC(SYS_fcntl, p[1], F_SETFL, O_NONBLOCK);
        open_fd(p[0]);
        open_fd(p[1]);
    }

    open_fd(SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0));
    open_fd(SC(SYS_socket, AF_INET, SOCK_DGRAM, 0));
    open_fd(SC(SYS_socket, AF_INET, SOCK_STREAM, 0));

    /* a listening socket, and a connection to it */
    struct sockaddr_in in = {0};
    in.sin_family = AF_INET;
    in.sin_port = 0x3930;
    in.sin_addr.s_addr = 0x0100007F;

    int l = SC(SYS_socket, AF_INET, SOCK_STREAM, 0);
    if (l >= 0 && SC(SYS_bind, l, &in, sizeof(in)) == 0 && SC(SYS_listen, l, 4, 0) == 0) {
        int c = SC(SYS_socket, AF_INET, SOCK_STREAM, 0);
        SC(SYS_fcntl, c, F_SETFL, O_NONBLOCK);
        SC(SYS_connect, c, &in, sizeof(in));
        open_fd(c);
        SC(SYS_fcntl, l, F_SETFL, O_NONBLOCK);
        open_fd(SC(SYS_accept, l, 0, 0));
    }
    open_fd(l);

    for (unsigned i = 0; i < sizeof(longpath) - 1; ++i)
        longpath[i] = i % 64 == 0 ? '/' : 'p';
}

static void __attribute__((noreturn)) worker(uint64_t seed)
{
    struct rng r;
    rng_seed(&r, seed);

    if (trace)
        SC(SYS_fcntl, aq_out, F_DUPFD, FD_TRACE);
    SC(SYS_close, aq_out, 0, 0);

    /* a session of its own: kill(0, ...) and its process group's signals
     * stay in it */
    SC(SYS_setsid, 0, 0, 0);

    scratch_map();

    for (unsigned i = 0; i < BUF_SIZE; ++i)
        buf[i] = (char) rnd(&r);

    setup();

    if (rndn(&r, 2)) {
        SC(SYS_setgid, 1000, 0, 0);
        SC(SYS_setuid, 1000, 0, 0);
    }

    /* two more threads, the same calls at the same time, sometimes */
    if (rndn(&r, 3) == 0) {
        for (int t = 0; t < 2; ++t) {
            thread_seeds[t] = rnd(&r);
            uptr entry = t ? (uptr) fuzz_thread1 : (uptr) fuzz_thread0;
            struct aquila_thread_args ta = {(uptr) &stacks[t][STACK_SIZE], entry, 0, 0, 0};
            SC(SYS_thread_create, &ta, 0, 0);
        }
    }

    alarm_arm();
    fuzz(&r, ncalls);
    SC(SYS_exit, 0, 0, 0);
    for (;;);
}

/*
 * init
 */

static unsigned long now_ms(void)
{
    struct aquila_timespec ts = {0};
    SC(SYS_clock_gettime, CLOCK_MONOTONIC, &ts, 0);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(long ms)
{
    struct aquila_timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    SC(SYS_nanosleep, &ts, 0, 0);
}

static char cmdline[1024];

/* name=value on the kernel's command line (/proc/cmdline): its number,
 * or def; *set if the name is there at all */
static unsigned long option(const char *name, unsigned long def, int *set)
{
    const char *s = cmdline;
    int len = aq_strlen(name);

    if (set)
        *set = 0;

    for (int i = 0; s && s[i]; ++i) {
        if ((i && s[i - 1] != ' ' && s[i - 1] != '\n') || s[i] != name[0])
            continue;

        int j = 0;
        while (j < len && s[i + j] == name[j])
            ++j;

        char e = s[i + j];
        if (j < len || (e && e != ' ' && e != '\n' && e != '='))
            continue;

        if (set)
            *set = 1;

        if (e != '=')
            return def;

        unsigned long v = 0;
        for (const char *p = &s[i + j + 1]; *p >= '0' && *p <= '9'; ++p)
            v = v * 10 + *p - '0';
        return v;
    }

    return def;
}

static void child_exits_7(void)
{
    SC(SYS_exit, 7, 0, 0);
}

void aq_main(void)
{
    aq_begin("fuzz");

    static struct { char *dev, *opt; } no_args = {0, 0};
    aq_mount("procfs", "/proc", &no_args);
    aq_mount("tmpfs", "/tmp", &no_args);

    int fd = SC(SYS_open, "/proc/cmdline", O_RDONLY, 0);
    if (fd >= 0) {
        SC(SYS_read, fd, cmdline, sizeof(cmdline) - 1);
        SC(SYS_close, fd, 0, 0);
    }

    first_seed = option("fuzz.seed", 1, 0);
    max_workers = option("fuzz.workers", 0, 0);
    secs = option("fuzz.secs", 30, 0);
    ncalls = option("fuzz.calls", 2000, 0);
    option("fuzz.trace", 0, &trace);

    static const struct { const char *path; uptr dev; } nodes[] = {
        {"/dev/null", DEV(1, 3)}, {"/dev/zero", DEV(1, 5)},
        {"/dev/full", DEV(1, 7)}, {"/dev/urandom", DEV(1, 9)},
        {"/dev/kmsg", DEV(1, 11)}, {"/dev/ptmx", DEV(5, 2)},
        {"/dev/tty", DEV(5, 0)}, {"/dev/tty1", DEV(4, 1)},
        {"/dev/vcs1", DEV(7, 1)}, {"/dev/fb0", DEV(29, 0)},
    };

    unsigned long start = now_ms(), workers = 0, exited = 0, signaled = 0, timedout = 0;
    int stuck = 0;

    for (uint64_t seed = first_seed; !max_workers || workers < max_workers; ++seed) {
        if (now_ms() - start > secs * 1000)
            break;

        /* the device nodes, each time: a worker may have removed them */
        for (unsigned i = 0; i < sizeof(nodes) / sizeof(nodes[0]); ++i)
            SC(SYS_mknod, nodes[i].path, S_IFCHR | 0666, nodes[i].dev);

        aq_puts("fuzz: seed ");
        putdec(aq_out, seed);
        aq_puts("\n");

        int pid = SC(SYS_fork, 0, 0, 0);
        if (pid == 0)
            worker(seed);
        if (pid < 0) {
            check("fork a worker", pid, 0);
            break;
        }

        ++workers;

        /* wait for it, 3 s at most; then SIGKILL, which must end it */
        unsigned long t0 = now_ms();
        int killed = 0, done = 0;

        while (!done) {
            int status;
            int w;

            /* its children, which init inherits, and it */
            while ((w = SC(SYS_waitpid, -1, &status, WNOHANG)) > 0) {
                if (w != pid)
                    continue;
                done = 1;
                if (killed) {
                    ++timedout;
                    aq_puts("fuzz: seed ");
                    putdec(aq_out, seed);
                    aq_puts(": stopped\n");
                } else if (status & 0x7F) {
                    ++signaled;
                    aq_puts("fuzz: seed ");
                    putdec(aq_out, seed);
                    aq_puts(": signal ");
                    putdec(aq_out, status & 0x7F);
                    aq_puts("\n");
                } else {
                    ++exited;
                    aq_puts("fuzz: seed ");
                    putdec(aq_out, seed);
                    aq_puts(": done\n");
                }
            }

            if (done)
                break;

            unsigned long t = now_ms() - t0;

            if (!killed && t > 3000) {
                SC(SYS_kill, pid, SIGKILL, 0);
                killed = 1;
            } else if (killed && t > 13000) {
                aq_puts("fuzz: seed ");
                putdec(aq_out, seed);
                aq_puts(": SIGKILL didn't end it in 10 s\n");
                stuck = 1;
                break;
            }

            sleep_ms(5);
        }

        if (stuck)
            break;
    }

    aq_puts("fuzz: ");
    putdec(aq_out, workers);
    aq_puts(" workers: ");
    putdec(aq_out, exited);
    aq_puts(" made their calls, ");
    putdec(aq_out, signaled);
    aq_puts(" were killed by a signal, ");
    putdec(aq_out, timedout);
    aq_puts(" were stopped after 3 s\n");

    check("workers ran", workers > 0, 1);
    check("every worker ended when killed", stuck, 0);
    check("fork and exit, after", aq_child_status(child_exits_7), 7 << 8);

    aq_end();
}
