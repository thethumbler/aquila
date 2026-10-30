/*
 * aarch64: the generic kernel on a second architecture (milestone 1)
 *
 * The port's own pieces, through what the kernel does with them: user
 * mode and svc system calls, the console (/dev/console: the PL011),
 * fork and copy on write, exec with arguments, pipes, anonymous memory,
 * page faults (a bad address: SIGSEGV), copies from user memory that
 * fail (EFAULT), a signal handler and sigreturn, the tick (nanosleep, the
 * clock), uname's machine, and the FP/SIMD registers (each thread's own,
 * across switches, forks and a signal handler that uses them). Then a
 * device on PCI Express (ECAM): the e1000, its BARs and interrupt the
 * kernel's to give, an echo request through it to QEMU's gateway.
 *
 * Run as /init; run again by exec with an argument ("exec-child"), it
 * exits 0x55 at once.
 */

#include "../lib/aqtest.h"
#include <aquila/mman.h>
#include <aquila/utsname.h>
#include <aquila/in.h>
#include <aquila/socket.h>

static int eq(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; ++i)
        if (a[i] != b[i])
            return 0;

    return 1;
}

static volatile int got_signal, cow;

static void handler(int sig)
{
    got_signal = sig;
}

static void child_exits_7(void)
{
    SC(SYS_exit, 7, 0, 0);
}

static void child_writes(void)
{
    cow = 2;
    SC(SYS_exit, cow, 0, 0);
}

static void child_faults(void)
{
    *(volatile int *) 0x10 = 1;
}

static void child_execs(void)
{
    char *argv[] = {"/init", "exec-child", 0};
    char *envp[] = {0};
    SC(SYS_execve, "/init", argv, envp);
}

/* FP: a sum the other threads' registers would spoil */
static double fp_sum(int n, double step)
{
    volatile double s = 0;

    for (int i = 0; i < n; ++i)
        s += step;

    return s;
}

static volatile double fp_in_handler;

static void fp_handler(int sig)
{
    fp_in_handler = fp_sum(1000, 0.5);     /* the handler's own FP use */
}

static void child_fp(void)
{
    /* meanwhile the parent sums too: both are preempted, both right */
    SC(SYS_exit, fp_sum(2000000, 0.25) == 500000.0 ? 0 : 1, 0, 0);
}

/* the kernel's struct timeval */
struct tv {
    uint64_t sec;
    long usec;
};

struct icmp {
    uint8_t  type, code;
    uint16_t checksum, id, seq;
    char data[32];
};

static uint16_t htons(uint16_t v) { return (uint16_t) (v << 8 | v >> 8); }

/** an echo request through a ping socket to `addr' (a.b.c.d, network
 * order), its reply: 1 if it came, as sent */
static int ping(uint32_t addr)
{
    int fd = SC(SYS_socket, AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    struct icmp req = {8, 0, 0, 0, htons(1), "aquila ping data, 32 bytes long"}, rep;
    struct sockaddr_in to = {0}, from;
    struct tv tv = {5, 0};
    unsigned int len = sizeof(to);

    SC5(SYS_setsockopt, fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    to.sin_family = AF_INET;
    to.sin_addr.s_addr = addr;

    struct aquila_socket_io_args out = {fd, &req, sizeof(req), 0, &to, &len};

    if (SC(SYS_send, &out, 0, 0) != sizeof(req))
        return 0;

    len = sizeof(from);
    struct aquila_socket_io_args in = {fd, &rep, sizeof(rep), 0, &from, &len};
    int n = SC(SYS_recv, &in, 0, 0);
    SC(SYS_close, fd, 0, 0);

    return n == sizeof(rep) && rep.type == 0 && rep.seq == htons(1) &&
           eq(rep.data, req.data, 32) && from.sin_addr.s_addr == addr;
}

static uint64_t now_ms(void)
{
    struct aquila_timespec ts = {0, 0};
    SC(SYS_clock_gettime, CLOCK_MONOTONIC, &ts, 0);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void aq_main(void)
{
    /* exec'd by child_execs(): its arguments came, it's done */
    if (aq_argc == 2 && eq(aq_argv[1], "exec-child", 11))
        SC(SYS_exit, 0x55, 0, 0);

    aq_begin("aarch64");

    /* user mode, entered as the ABI says */
    check("getpid: init", SC(SYS_getpid, 0, 0, 0), 1);
    check("the stack 16-byte aligned at entry", aq_entry_esp & 15, 0);
    check("argc", aq_argc, 1);
    aq_check_initial_stack();

    /* the console: /dev/console, the first uart without virtual
     * terminals (the kernel made the node) */
    int con = SC(SYS_open, "/dev/console", O_WRONLY, 0);
    check("open /dev/console", con >= 0, 1);
    check("write to the console", SC(SYS_write, con, "hello from aarch64 user mode\n", 29), 29);

    struct utsname u;
    check("uname", SC(SYS_uname, &u, 0, 0), 0);
    check("uname: aarch64", eq(u.machine, "aarch64", 8), 1);
    aq_puts("uname: ");
    aq_puts(u.sysname);
    aq_puts(" ");
    aq_puts(u.release);
    aq_puts(" ");
    aq_puts(u.machine);
    aq_puts("\n");

    /* processes */
    check("fork, exit, waitpid", aq_child_status(child_exits_7), 7 << 8);

    cow = 1;
    check("fork: the child's copy", aq_child_status(child_writes), 2 << 8);
    check("fork: copy on write, the parent's as it was", cow, 1);

    check("exec, its arguments", aq_child_status(child_execs), 0x55 << 8);
    check("a bad address: SIGSEGV", aq_child_status(child_faults) & 0x7F, SIGSEGV);

    /* pipes */
    int p[2] = {-1, -1};
    char buf[16] = {0};
    check("pipe", SC(SYS_pipe, p, 0, 0), 0);
    SC(SYS_write, p[1], "through", 7);
    check("pipe: read", SC(SYS_read, p[0], buf, 7) == 7 && eq(buf, "through", 7), 1);

    /* memory */
    void *m = 0;
    struct aquila_mmap_args ma = {0, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
    check("mmap anonymous", SC(SYS_mmap, &ma, &m, 0), 0);
    volatile char *mem = m;
    check("mmap: zeros", mem[0] | mem[4096] | mem[8191], 0);
    mem[0] = 'a';
    mem[8191] = 'z';
    check("mmap: written, read back", mem[0] == 'a' && mem[8191] == 'z', 1);

    /* a large mapping with one page in it: fork() and munmap() go over
     * its page tables, not its 4 billion pages */
    struct aquila_mmap_args big = {0, 1UL << 44, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
    check("mmap 16 TiB", SC(SYS_mmap, &big, &m, 0), 0);
    uint64_t b0 = now_ms();
    ((volatile char *) m)[12345] = 1;
    int bc = SC(SYS_fork, 0, 0, 0);
    if (!bc)
        SC(SYS_exit, 0, 0, 0);
    int bst = -1;
    SC(SYS_waitpid, bc, &bst, 0);
    check("munmap 16 TiB", SC(SYS_munmap, m, 1UL << 44, 0), 0);
    check("fork and munmap of 16 TiB with a page in it: quick", now_ms() - b0 < 1000, 1);

    /* copies from user memory that fail */
    check("write from a kernel address: EFAULT", SC(SYS_write, con, KADDR, 4), -EFAULT);
    SC(SYS_write, p[1], "x", 1);
    check("read into nowhere: EFAULT", SC(SYS_read, p[0], 0x10, 4), -EFAULT);

    /* signals: a handler, and back (sigreturn) */
    struct aquila_sigaction sa = {(uptr) handler, 0, 0};
    check("sigaction", SC(SYS_sigaction, SIGUSR1, &sa, 0), 0);
    SC(SYS_kill, SC(SYS_getpid, 0, 0, 0), SIGUSR1, 0);
    check("a signal's handler ran, and returned", got_signal, SIGUSR1);

    /* the tick: sleeping, and the clock */
    uint64_t t0 = now_ms();
    struct aquila_timespec d = {0, 100000000};
    check("nanosleep 100 ms", SC(SYS_nanosleep, &d, 0, 0), 0);
    uint64_t slept = now_ms() - t0;
    check("the clock: 100 ms passed", slept >= 100 && slept < 1000, 1);

    /* the FP/SIMD registers: switched lazily, each thread its own */
    volatile double a = 1.5;
    check("FP: arithmetic", a * a + 0.25 == 2.5, 1);

    int fc = SC(SYS_fork, 0, 0, 0);
    if (!fc)
        child_fp();
    double mine = fp_sum(2000000, 0.125);
    int fst = -1;
    SC(SYS_waitpid, fc, &fst, 0);
    check("FP: a child's, while preempted", fst, 0);
    check("FP: the parent's, while preempted", mine == 250000.0, 1);

    struct aquila_sigaction fsa = {(uptr) fp_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR2, &fsa, 0);
    volatile double kept = 3.25;
    double before = kept * 2;
    SC(SYS_kill, SC(SYS_getpid, 0, 0, 0), SIGUSR2, 0);
    check("FP: a handler that uses them", fp_in_handler == 500.0, 1);
    check("FP: what it interrupted, as it was", before == 6.5 && kept * 2 == 6.5, 1);

    /* the e1000 on PCI Express, 10.0.2.15 (ip=): the gateway answers */
    check("PCI: the e1000, a ping to the gateway", ping(10 | 0 << 8 | 2 << 16 | 2u << 24), 1);

    aq_end();
}
