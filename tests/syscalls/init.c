/*
 * System call argument validation and semantics
 *
 * - pointers into kernel space must be rejected with EFAULT
 * - descriptors, signal numbers and sizes must be range checked
 * - signals, wait, threads, pipes, unix sockets, exec of bad ELF files
 * - files: the *at() calls, links, rename, permissions and attributes;
 *   paths walked vnode by vnode (.. after a link, the cwd and an open
 *   directory renamed, mounts, search permission, trailing slashes)
 * - nanosleep and clocks
 * - FIFOs
 * - sessions and controlling terminals, on a pty
 * - stopping and continuing processes, the terminal line discipline
 * - select, poll and terminal read timeouts (VTIME); nanosleep, poll and
 *   select stopped and continued keep their deadline
 * - flock
 * - anonymous memory (mmap), shutdown() of sockets, the interval timer
 * - SA_RESTART, SA_NOCLDWAIT and an ignored SIGCHLD, getrandom
 * - signals delivered on the way back to user mode: the FPU and SSE state
 *   kept across a handler, the handler's signal blocked (SA_NODEFER),
 *   sa_mask, SA_RESETHAND, the mask restored, nested handlers,
 *   sigsuspend's EINTR and mask, a read stopped and continued is made
 *   again, sigreturn with a forged frame stays in user mode, and one
 *   returning past user space kills (SIGSEGV), as is a handler there
 *   refused; a forked
 *   child has its parent's SSE state
 * - orphans: init's, a stopped orphaned group's SIGHUP and SIGCONT, the
 *   terminal's stops and reads in an orphaned group
 * - open files: shared by dup() and fork() (offset, status flags), kept
 *   open by a call using one
 * - the FPU of threads that start while another has it: their own
 * - inode numbers whole: stat's are readdir's (the initramfs's are
 *   pointers, they were cut to 16 bits)
 * - exec ends the other threads (one asleep, one ready to run): the new
 *   image runs alone, nothing of the old one writes to its memory
 * - user memory that isn't mapped: EFAULT, the process goes on, a read's
 *   bytes stay, a buffer running into it is used up to it
 * - virtual terminals: KD* and VT_* ioctls, switching, VT_PROCESS
 * - a terminal's output modes (ONLCR, ONLRET, ONOCR, OCRNL)
 */

#include "../lib/aqtest.h"

#include <aquila/dirent.h>
#include <aquila/mman.h>
#include <aquila/un.h>
#include <aquila/termios.h>
#include <aquila/wait.h>
#include <aquila/poll.h>
#include <aquila/reboot.h>
#include <aquila/kd.h>
#include <aquila/vt.h>
#include <aquila/keyboard.h>
#include <aquila/time.h>

static volatile int got_sig = 0;
static void handler(int sig) { got_sig = sig; }

/* VT_PROCESS: the signals asking to release the terminal, and telling it
 * is back */
static volatile int vt_rel = 0, vt_acq = 0;

static volatile int alarms = 0;
static void alarm_handler(int sig) { alarms++; (void) sig; }
static void vt_handler(int sig) { if (sig == SIGUSR1) vt_rel++; else vt_acq++; }

/* "/dev/tty<n>" */
static char *vt_path(int n)
{
    static char path[] = "/dev/ttyN";
    path[8] = '0' + n;
    return path;
}

static int vt_active(int fd)
{
    struct vt_stat vs = {0};
    SC(SYS_ioctl, fd, VT_GETSTATE, &vs);
    return vs.v_active;
}
static void child_handler(int sig) { SC(SYS_exit, 7, 0, 0); (void) sig; }


static volatile int thread_ran = 0, thread_self = 0;
static void thr(void *u, void *a) { (void) u; (void) a; thread_self = SC(SYS_thread_self, 0, 0, 0); thread_ran = 1; SC(SYS_thread_exit, 7, 0, 0); for (;;); }
static char tstack[8192] __attribute__((aligned(16)));

static unsigned char bad_elf[52 + 32];

static int hup_pipe = -1;
static void hup_handler(int sig) { SC(SYS_write, hup_pipe, "H", 1); SC(SYS_exit, 0, 0, 0); (void) sig; }

/* "/dev/pts/<n>" */
static char *pts_path(int n)
{
    static char path[16] = "/dev/pts/";
    int i = 9;

    if (n >= 10)
        path[i++] = '0' + n / 10;
    path[i++] = '0' + n % 10;
    path[i] = 0;
    return path;
}

/* a new session with a pty as its controlling terminal; exits with the
 * failed step */
/*
 * The terminal's output modes (OPOST): what the slave writes, as the
 * master reads it. ONLRET: a NL is output (it returns the carriage too);
 * ONOCR: no CR in the first column, the output's (not the line being
 * typed); OCRNL: a CR as NL.
 */
static int pty_out(int master, int slave, unsigned oflag, const char *in, const char *want)
{
    struct termios t;
    char got[32];
    int n = 0, len = 0;

    SC(SYS_ioctl, slave, TCGETS, &t);
    t.c_oflag = OPOST | oflag;
    SC(SYS_ioctl, slave, TCSETS, &t);

    while (in[n])
        n++;

    SC(SYS_write, slave, in, n);
    aq_spin(100000);

    int r = SC(SYS_read, master, got, sizeof(got) - 1);
    got[r > 0 ? r : 0] = 0;

    while (want[len])
        len++;

    if (r != len)
        return 0;

    for (int i = 0; i < len; ++i)
        if (got[i] != want[i])
            return 0;

    return 1;
}

static void tty_output_modes(void)
{
    int n = -1;
    int master = SC(SYS_open, "/dev/ptmx", O_RDWR | O_NOCTTY, 0);
    SC(SYS_ioctl, master, TIOCGPTN, &n);
    int slave = SC(SYS_open, pts_path(n), O_RDWR | O_NOCTTY, 0);

    check("tty: ONLCR", pty_out(master, slave, ONLCR, "a\nb", "a\r\nb"), 1);
    check("tty: ONLRET keeps the NL", pty_out(master, slave, ONLRET, "a\nb", "a\nb"), 1);
    check("tty: ONOCR, a CR in the first column", pty_out(master, slave, ONLCR | ONOCR, "\n\rx\r", "\r\nx\r"), 1);
    check("tty: ONOCR after ONLRET's NL", pty_out(master, slave, ONLRET | ONOCR, "ab\n\r", "ab\n"), 1);
    check("tty: OCRNL", pty_out(master, slave, OCRNL, "a\rb", "a\nb"), 1);

    /* flow control (IXON, on by default): ^S stops the output, a write
     * waits; ^Q starts it, the write goes */
    char fc[4];
    SC(SYS_write, master, "\x13", 1);
    int w = SC(SYS_fork, 0, 0, 0);
    if (!w) SC(SYS_exit, SC(SYS_write, slave, "go", 2) == 2 ? 0 : 1, 0, 0);
    aq_spin(30000000);
    SC(SYS_fcntl, master, F_SETFL, O_NONBLOCK);
    check("tty: ^S stops the output", SC(SYS_read, master, fc, 4), -EAGAIN);
    SC(SYS_write, master, "\x11", 1);
    int ws = -1;
    SC(SYS_waitpid, w, &ws, 0);
    check("tty: ^Q starts it", SC(SYS_read, master, fc, 4) == 2 && fc[0] == 'g' && ws == 0, 1);
    SC(SYS_fcntl, master, F_SETFL, 0);

    SC(SYS_close, slave, 0, 0);
    SC(SYS_close, master, 0, 0);

    /* the master writes three times what the slave's input buffer holds
     * (4 KiB) in one write, while the slave reads it: the writer sleeps
     * whenever the buffer is full, and each read must wake it up again */
    master = SC(SYS_open, "/dev/ptmx", O_RDWR | O_NOCTTY, 0);
    SC(SYS_ioctl, master, TIOCGPTN, &n);
    slave = SC(SYS_open, pts_path(n), O_RDWR | O_NOCTTY, 0);

    struct termios raw;
    SC(SYS_ioctl, slave, TCGETS, &raw);
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    SC(SYS_ioctl, slave, TCSETS, &raw);

    static char big[12000], got[512];

    for (int i = 0; i < (int) sizeof(big); ++i)
        big[i] = 'a' + i % 26;

    int writer = SC(SYS_fork, 0, 0, 0);
    if (!writer)
        SC(SYS_exit, SC(SYS_write, master, big, sizeof(big)) == sizeof(big) ? 0 : 1, 0, 0);

    int total = 0, same = 1;

    while (total < (int) sizeof(big)) {
        /* nothing for 2 s: the writer is stuck */
        struct pollfd pf = {slave, POLLIN, 0};
        if (SC(SYS_poll, &pf, 1, 2000) <= 0)
            break;

        int r = SC(SYS_read, slave, got, sizeof(got));
        if (r <= 0)
            break;

        for (int i = 0; i < r; ++i)
            if (got[i] != big[total + i])
                same = 0;

        total += r;
    }

    check("pty: a write bigger than the buffer, read as it comes", total == (int) sizeof(big) && same, 1);

    SC(SYS_kill, writer, SIGKILL, 0);
    int wst = -1;
    SC(SYS_waitpid, writer, &wst, 0);

    SC(SYS_close, slave, 0, 0);
    SC(SYS_close, master, 0, 0);

    /* another node of the serial port the results go to: opened and
     * closed, the first one (aq_out) stays as it was (its vnode used to
     * be closed in its place) */
    SC(SYS_mknod, "/dev/ttyS0b", S_IFCHR | 0666, DEV(4, 64));

    for (int i = 0; i < 4; ++i) {
        int fd = SC(SYS_open, "/dev/ttyS0b", O_RDWR | O_NOCTTY, 0);
        SC(SYS_close, fd, 0, 0);
    }

    struct aquila_stat sb;
    check("tty: another node of the serial port, opened and closed",
          SC(SYS_fstat, aq_out, &sb, 0) == 0 && SC(SYS_write, aq_out, "", 0) == 0, 1);
    SC(SYS_unlink, "/dev/ttyS0b", 0, 0);
}

static void pty_session(void)
{
    int me = SC(SYS_getpid, 0, 0, 0), pgid = 0, n = -1;

    if (SC(SYS_setsid, 0, 0, 0) != me) SC(SYS_exit, 1, 0, 0);
    if (SC(SYS_open, "/dev/tty", O_RDWR, 0) != -ENXIO) SC(SYS_exit, 2, 0, 0);

    int master = SC(SYS_open, "/dev/ptmx", O_RDWR, 0);
    if (master < 0 || SC(SYS_ioctl, master, TIOCGPTN, &n) || n < 0) SC(SYS_exit, 3, 0, 0);

    /* opening the slave makes it the controlling terminal */
    int slave = SC(SYS_open, pts_path(n), O_RDWR, 0);
    if (slave < 0) SC(SYS_exit, 4, 0, 0);
    if (SC(SYS_ioctl, slave, TIOCGPGRP, &pgid) || pgid != me) SC(SYS_exit, 5, 0, 0);

    /* /dev/tty is that pty */
    int tty = SC(SYS_open, "/dev/tty", O_WRONLY, 0);
    if (tty < 0) SC(SYS_exit, 6, 0, 0);
    char out[8] = {0};
    SC(SYS_write, tty, "via", 3);
    if (SC(SYS_read, master, out, 8) != 3 || out[0] != 'v') SC(SYS_exit, 7, 0, 0);

    /* a foreground group that goes away leaves none */
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        SC(SYS_setpgid, 0, 0, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    aq_spin(10000000);
    if (SC(SYS_ioctl, slave, TIOCSPGRP, &c)) SC(SYS_exit, 8, 0, 0);
    int st;
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_ioctl, slave, TIOCGPGRP, &pgid);
    if (pgid != 0x7fffffff) SC(SYS_exit, 9, 0, 0);
    SC(SYS_ioctl, slave, TIOCSPGRP, &me);

    /* the end of the session hangs up the foreground group: a child left
     * in it writes to hup_pipe when it gets SIGHUP */
    int ready[2];
    SC(SYS_pipe, ready, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_sigaction hsa = {(uptr) hup_handler, 0, 0};
        SC(SYS_sigaction, SIGHUP, &hsa, 0);
        SC(SYS_write, ready[1], "R", 1);
        for (;;)
            SC(SYS_getpid, 0, 0, 0);
    }
    char r;
    SC(SYS_read, ready[0], &r, 1);
    SC(SYS_exit, 0, 0, 0);
}

static void spin_forever(void)
{
    for (;;)
        SC(SYS_getpid, 0, 0, 0);
}

static void sigtstp_handler(int sig) { SC(SYS_exit, 9, 0, 0); (void) sig; }

/* the line discipline of a pty that is the controlling terminal of a new
 * session; exits with the failed step */
static void line_discipline(void)
{
    int n = -1, me = SC(SYS_getpid, 0, 0, 0), st;
    char buf[32];

    SC(SYS_setsid, 0, 0, 0);
    int master = SC(SYS_open, "/dev/ptmx", O_RDWR, 0);
    SC(SYS_ioctl, master, TIOCGPTN, &n);
    int slave = SC(SYS_open, pts_path(n), O_RDWR, 0);
    if (master < 0 || slave < 0) SC(SYS_exit, 1, 0, 0);

    /* ^D: the line so far without a newline, then end-of-file */
    SC(SYS_write, master, "ab\x04", 3);
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 2 || buf[1] != 'b') SC(SYS_exit, 2, 0, 0);
    SC(SYS_write, master, "\x04", 1);
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 0) SC(SYS_exit, 3, 0, 0);

    /* erase (DEL) and kill (^U) */
    SC(SYS_write, master, "q\x7fz\x15xy\n", 7);
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 3 || buf[0] != 'x' || buf[2] != '\n') SC(SYS_exit, 4, 0, 0);

    /* more than one line in a write: nothing is lost */
    SC(SYS_write, master, "1\n2\n", 4);
    int got = 0, r;
    while (got < 4 && (r = SC(SYS_read, slave, buf + got, sizeof(buf) - got)) > 0)
        got += r;
    if (got != 4 || buf[2] != '2') SC(SYS_exit, 5, 0, 0);

    /* non-canonical with VMIN 0 doesn't wait */
    struct termios t, raw;
    SC(SYS_ioctl, slave, TCGETS, &t);
    raw = t;
    raw.c_lflag &= ~ICANON;
    raw.c_cc[VMIN] = 0;
    SC(SYS_ioctl, slave, TCSETS, &raw);
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 0) SC(SYS_exit, 6, 0, 0);
    SC(SYS_write, master, "k", 1);
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 1 || buf[0] != 'k') SC(SYS_exit, 7, 0, 0);
    SC(SYS_ioctl, slave, TCSETS, &t);

    /* a background reader is stopped by SIGTTIN */
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        SC(SYS_setpgid, 0, 0, 0);
        SC(SYS_read, slave, buf, 1);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, WUNTRACED);
    if ((st & 0xff) != 0x7f || (st >> 8) != SIGTTIN) SC(SYS_exit, 8, 0, 0);
    SC(SYS_kill, c, SIGKILL, 0);
    SC(SYS_waitpid, c, &st, 0);

    /* ^Z stops the foreground group, ^C kills it */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        SC(SYS_setpgid, 0, 0, 0);
        spin_forever();
    }
    SC(SYS_setpgid, c, c, 0);
    SC(SYS_ioctl, slave, TIOCSPGRP, &c);
    SC(SYS_write, master, "\x1a", 1);
    SC(SYS_waitpid, c, &st, WUNTRACED);
    if ((st & 0xff) != 0x7f || (st >> 8) != SIGTSTP) SC(SYS_exit, 10, 0, 0);
    SC(SYS_kill, c, SIGCONT, 0);
    SC(SYS_write, master, "\x03", 1);
    SC(SYS_waitpid, c, &st, 0);
    if ((st & 0x7f) != SIGINT) SC(SYS_exit, 11, 0, 0);
    SC(SYS_ioctl, slave, TIOCSPGRP, &me);

    SC(SYS_exit, 0, 0, 0);
}

/* select() sets, as the kernel takes them (FD_SETSIZE 64) */
struct fdset { unsigned long bits[2]; };

static int do_select(int nfds, struct fdset *r, struct fdset *w, long usec)
{
    struct { u32 sec_lo, sec_hi; long usec; } tv = {usec / 1000000, 0, usec % 1000000};
    struct aquila_select_args a = {nfds, r, w, 0, usec < 0 ? 0 : &tv};
    return SC(SYS_select, &a, 0, 0);
}

static int now_ms(void);

/* a raw read on a pty with VMIN 0 and VTIME 3 waits 300ms for input;
 * exits with the failed step */
static void read_timeout(void)
{
    int n = -1;
    char buf[8];

    SC(SYS_setsid, 0, 0, 0);
    int master = SC(SYS_open, "/dev/ptmx", O_RDWR, 0);
    SC(SYS_ioctl, master, TIOCGPTN, &n);
    int slave = SC(SYS_open, pts_path(n), O_RDWR, 0);

    struct termios t;
    SC(SYS_ioctl, slave, TCGETS, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 3;
    SC(SYS_ioctl, slave, TCSETS, &t);

    int t0 = now_ms();
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 0) SC(SYS_exit, 1, 0, 0);
    int waited = now_ms() - t0;
    if (waited < 280 || waited > 1000) SC(SYS_exit, 2, 0, 0);

    /* input before the time is up ends the wait */
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_timespec d = {0, 100000000};
        SC(SYS_nanosleep, &d, 0, 0);
        SC(SYS_write, master, "z", 1);
        SC(SYS_exit, 0, 0, 0);
    }
    t0 = now_ms();
    if (SC(SYS_read, slave, buf, sizeof(buf)) != 1 || buf[0] != 'z') SC(SYS_exit, 3, 0, 0);
    if (now_ms() - t0 >= 280) SC(SYS_exit, 4, 0, 0);

    /* select sees the master readable once the terminal writes */
    struct fdset r = {{1UL << master, 0}};
    if (do_select(master + 1, &r, 0, 0) != 0) SC(SYS_exit, 5, 0, 0);
    SC(SYS_write, slave, "out", 3);
    r.bits[0] = 1UL << master;
    if (do_select(master + 1, &r, 0, 0) != 1) SC(SYS_exit, 6, 0, 0);

    SC(SYS_exit, 0, 0, 0);
}

/* CLOCK_MONOTONIC in milliseconds */
static int now_ms(void)
{
    struct aquila_timespec ts;
    SC(SYS_clock_gettime, CLOCK_MONOTONIC, &ts, 0);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#define ST(buf) ((struct aquila_stat *) (buf))

/* as an unprivileged user (uid 1000) in /tmp, exits with the failed step */
static void unprivileged_files(void)
{
    char st[128];

    if (SC(SYS_setgid, 100, 0, 0) || SC(SYS_setuid, 1000, 0, 0)) SC(SYS_exit, 1, 0, 0);

    /* /tmp/rootdir is root's, 0755 */
    if (SC(SYS_open, "/tmp/rootdir/new", O_CREAT | O_WRONLY, 0644) != -EACCES) SC(SYS_exit, 2, 0, 0);
    if (SC(SYS_unlink, "/tmp/rootdir/file", 0, 0) != -EACCES) SC(SYS_exit, 3, 0, 0);
    if (SC(SYS_chmod, "/tmp/rootdir/file", 0777, 0) != -EPERM) SC(SYS_exit, 4, 0, 0);
    if (SC5(SYS_utimensat, AT_FDCWD, "/tmp/rootdir/file", 0, 0, 0) != -EACCES) SC(SYS_exit, 5, 0, 0);
    if (SC(SYS_mknod, "/tmp/dev", S_IFCHR | 0666, DEV(4, 64)) != -EPERM) SC(SYS_exit, 6, 0, 0);

    /* own files */
    int fd = SC(SYS_open, "/tmp/mine", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) SC(SYS_exit, 7, 0, 0);
    SC(SYS_fstat, fd, st, 0);
    if (ST(st)->st_uid != 1000 || ST(st)->st_gid != 100) SC(SYS_exit, 8, 0, 0);
    if (SC(SYS_fchmod, fd, 0600, 0)) SC(SYS_exit, 9, 0, 0);
    /* giving it away */
    if (SC(SYS_chown, "/tmp/mine", 0, -1) != -EPERM) SC(SYS_exit, 10, 0, 0);
    if (SC(SYS_chown, "/tmp/mine", -1, 100)) SC(SYS_exit, 11, 0, 0);
    if (SC(SYS_chown, "/tmp/mine", -1, 0) != -EPERM) SC(SYS_exit, 12, 0, 0);
    SC(SYS_close, fd, 0, 0);
    if (SC(SYS_unlink, "/tmp/mine", 0, 0)) SC(SYS_exit, 13, 0, 0);
    /* only root restarts the machine */
    if (SC(SYS_reboot, RB_AUTOBOOT, 0, 0) != -EPERM) SC(SYS_exit, 14, 0, 0);

    SC(SYS_exit, 0, 0, 0);
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        ++a, ++b;

    return *a == *b;
}

/* run as the interpreter of /tmp/script (see the execve tests): argc,
 * argv and envp are on the entry stack */
static void script_interpreter(void)
{
    char **argv = aq_argv;

    if (aq_argc != 4 || !str_eq(argv[1], "-x y"))
        return;

    SC(SYS_exit, str_eq(argv[0], "/init") && str_eq(argv[2], "/tmp/script") &&
                 str_eq(argv[3], "arg") ? 42 : 1, 0, 0);
}

/*
 * exec with other threads: they end, the new image (this program again,
 * "alone") runs by itself. They write where it checks a pattern: had one
 * gone on, in the new image (the same code), the pattern would change.
 */
static volatile unsigned char exec_pattern[16384];
static char exec_stacks[2][8192] __attribute__((aligned(16)));

static void exec_alone(void)
{
    char **argv = aq_argv;

    if (aq_argc != 2 || !str_eq(argv[1], "alone"))
        return;

    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < (int) sizeof(exec_pattern); ++i)
            exec_pattern[i] = i * 7 + round;

        aq_spin(200000);

        for (int i = 0; i < (int) sizeof(exec_pattern); ++i)
            if (exec_pattern[i] != (unsigned char) (i * 7 + round))
                SC(SYS_exit, 1, 0, 0);
    }

    SC(SYS_exit, 0x55, 0, 0);
}

static void exec_spinner(void *u, void *a)
{
    for (;;)
        exec_pattern[1000] = 1;
}

static void exec_sleeper(void *u, void *a)
{
    struct aquila_timespec ts = {0, 100000000};
    SC(SYS_nanosleep, &ts, 0, 0);

    for (;;)
        exec_pattern[2000] = 1;
}

static void exec_with_threads(void)
{
    void (*fns[2])(void *, void *) = {exec_spinner, exec_sleeper};

    for (int i = 0; i < 2; ++i) {
        struct aquila_thread_args ta = {(uptr) &exec_stacks[i][8192], (uptr) fns[i], 0, 0, 0};
        SC(SYS_thread_create, &ta, 0, 0);
    }

    aq_spin(1000000);

    char *argv[] = {"/init", "alone", 0}, *envp[] = {0};
    SC(SYS_execve, "/init", argv, envp);
    SC(SYS_exit, 9, 0, 0);
}

/*
 * Signals on the way back to user mode (x86_return_to_user)
 */

static volatile int depth, max_depth, count, got2, got2_during, nested;

static int self_pid(void) { return SC(SYS_getpid, 0, 0, 0); }

/* sends itself the signal again, once: blocked while it runs, unless
 * SA_NODEFER */
static void deep_handler(int sig)
{
    if (++depth > max_depth)
        max_depth = depth;

    if (++count == 1)
        SC(SYS_kill, self_pid(), sig, 0);

    depth--;
}

static void usr2_handler(int sig) { got2++; nested = depth; (void) sig; }

/* SIGUSR2 is in its sa_mask: not delivered while it runs */
static void masking_handler(int sig)
{
    SC(SYS_kill, self_pid(), SIGUSR2, 0);
    got2_during = got2;
    (void) sig;
}

/* in a handler, another signal's handler runs */
static void outer_handler(int sig)
{
    depth = 1;
    SC(SYS_kill, self_pid(), SIGUSR2, 0);
    depth = 0;
    (void) sig;
}

/* changes the SSE state: the interrupted code's must come back */
__attribute__((target("sse")))
static void sse_handler(int sig)
{
    static uint32_t round_down = 0x3F80;
    asm volatile("xorps %%xmm1, %%xmm1\n"
                 "ldmxcsr %0" :: "m"(round_down) : "xmm1");
    (void) sig;
}

/* xmm1 and MXCSR around a kill of itself (its handler runs before the
 * syscall returns): 1 if both are as they were */
__attribute__((target("sse")))
static int sse_kept(void)
{
    static float in[4] __attribute__((aligned(16))) = {1.5f, 2.5f, 3.5f, 4.5f};
    static float out[4] __attribute__((aligned(16)));
    uint32_t mx_before, mx_after;
    long r = SYS_kill;

#ifdef __x86_64__
    asm volatile("stmxcsr %[mb]\n"
                 "movaps %[in], %%xmm1\n"
                 "int $0x80\n"
                 "movaps %%xmm1, %[out]\n"
                 "stmxcsr %[ma]\n"
                 : [out] "=m"(out), [mb] "=m"(mx_before), [ma] "=m"(mx_after), "+a"(r)
                 : [in] "m"(in), "D"((uptr) self_pid()), "S"((uptr) SIGUSR1)
                 : "xmm1", "memory", "rcx", "rdx", "r8", "r9", "r10", "r11");
#else
    asm volatile("stmxcsr %[mb]\n"
                 "movaps %[in], %%xmm1\n"
                 "int $0x80\n"
                 "movaps %%xmm1, %[out]\n"
                 "stmxcsr %[ma]\n"
                 : [out] "=m"(out), [mb] "=m"(mx_before), [ma] "=m"(mx_after), "+a"(r)
                 : [in] "m"(in), "b"((uptr) self_pid()), "c"((uptr) SIGUSR1)
                 : "xmm1", "memory", "edx");
#endif

    for (int i = 0; i < 4; ++i)
        if (out[i] != in[i])
            return 0;

    return mx_before == mx_after;
}

/* xmm1 and MXCSR set, then fork: the child has them as the parent did
 * (exits 0 if so); the parent's MXCSR is put back, the child's pid */
__attribute__((target("sse")))
static int sse_fork(void)
{
    static float in[4] __attribute__((aligned(16))) = {5.5f, 6.5f, 7.5f, 8.5f};
    static float out[4] __attribute__((aligned(16)));
    uint32_t mx_set = 0x7F80, mx_before, mx_after;     /* round toward zero */
    long r = SYS_fork;

#ifdef __x86_64__
    asm volatile("stmxcsr %[mb]\n"
                 "ldmxcsr %[ms]\n"
                 "movaps %[in], %%xmm1\n"
                 "int $0x80\n"
                 "movaps %%xmm1, %[out]\n"
                 "stmxcsr %[ma]\n"
                 "ldmxcsr %[mb]\n"
                 : [out] "=m"(out), [mb] "=m"(mx_before), [ma] "=m"(mx_after), "+a"(r)
                 : [in] "m"(in), [ms] "m"(mx_set)
                 : "xmm1", "memory", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11");
#else
    asm volatile("stmxcsr %[mb]\n"
                 "ldmxcsr %[ms]\n"
                 "movaps %[in], %%xmm1\n"
                 "int $0x80\n"
                 "movaps %%xmm1, %[out]\n"
                 "stmxcsr %[ma]\n"
                 "ldmxcsr %[mb]\n"
                 : [out] "=m"(out), [mb] "=m"(mx_before), [ma] "=m"(mx_after), "+a"(r)
                 : [in] "m"(in), [ms] "m"(mx_set)
                 : "xmm1", "memory", "ebx", "ecx", "edx");
#endif

    if (!r) {
        int ok = mx_after == mx_set;

        for (int i = 0; i < 4; ++i)
            ok = ok && out[i] == in[i];

        SC(SYS_exit, ok ? 0 : 1, 0, 0);
    }

    return r;
}

/* the kernel's struct sigframe (arch/i386/sys/signal.c) */
struct test_regs {
#ifdef __x86_64__
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rcx, rdx, rax,
             vec, err, ip, cs, flags, sp, ss;
#else
    uint32_t edi, esi, ebp, ebx, ecx, edx, eax, gs, fs, es, ds, vec, err, ip, cs, flags, sp, ss;
#endif
};

struct test_frame {
    struct aquila_siginfo info;
    unsigned long mask;
    uint32_t fpu;
    struct test_regs regs;
    uint8_t fxsave[512] __attribute__((aligned(16)));
} __attribute__((aligned(16)));

static struct test_frame forged;
static char landing_stack[4096] __attribute__((aligned(16)));

/* where the forged frame goes: user mode's code segment, no IOPL, no
 * NT, interrupts on, whatever the frame said */
static void __attribute__((noreturn)) landing(void)
{
    uptr cs, flags;

    asm volatile("mov %%cs, %0" : "=r"(cs));
#ifdef __x86_64__
    asm volatile("pushfq; popq %0" : "=r"(flags));
#else
    asm volatile("pushfl; popl %0" : "=r"(flags));
#endif

    int ok = (cs & 0xffff) == (0x18 | 3) && !(flags & 0x3000) && !(flags & 0x4000) && (flags & 0x200);
    SC(SYS_exit, ok ? 0 : 1, 0, 0);
    for (;;);
}

/* sigreturn with the stack at `frame' (where a handler's return leaves
 * it) */
static void __attribute__((noreturn)) sigreturn_at(uptr frame)
{
#ifdef __x86_64__
    asm volatile("mov %0, %%rsp\n"
                 "mov %1, %%eax\n"
                 "int $0x80" :: "r"(frame), "i"(SYS_sigreturn) : "memory");
#else
    /* the handler's arguments are still above its stack: 16 bytes */
    asm volatile("mov %0, %%esp\n"
                 "mov %1, %%eax\n"
                 "int $0x80" :: "r"(frame - 16), "i"(SYS_sigreturn) : "memory");
#endif
    for (;;);
}

static void forged_sigreturn(void)
{
    forged.regs.ip = (uptr) landing;
    forged.regs.sp = (uptr) landing_stack + sizeof(landing_stack) - sizeof(uptr);
    forged.regs.cs = 0x08;                          /* the kernel's */
    forged.regs.ss = 0x10;
    forged.regs.flags = 0x3000 | 0x4000 | 0x2;      /* IOPL 3, NT, no IF */
#ifndef __x86_64__
    forged.regs.flags |= 0x20000;                   /* virtual 8086 */
    forged.regs.ds = forged.regs.es = forged.regs.fs = forged.regs.gs = 0x10;
#endif
    sigreturn_at((uptr) &forged);
}

/* past user space: on x86_64 not canonical, which iretq refuses */
#ifdef __x86_64__
#define PAST_USER   0x8000000000000000ul
#else
#define PAST_USER   KADDR
#endif

/* a frame as a handler's return finds it, but returning past user space */
static void past_user_sigreturn(void)
{
    forged.regs.ip = PAST_USER;
    forged.regs.sp = (uptr) landing_stack + sizeof(landing_stack) - sizeof(uptr);
    forged.regs.flags = 0x202;
    sigreturn_at((uptr) &forged);
}

static void garbage_sigreturn(void)
{
    /* nothing mapped there */
    sigreturn_at(0x10000);
}

/* user mode's %gs is its own: a null one, through interrupts and system
 * calls (the kernel finds its CPU with its own) */
static void user_gs(void)
{
    asm volatile("mov %0, %%gs" :: "r"(0));
    aq_spin(20000000);

    int ok = SC(SYS_getpid, 0, 0, 0) > 0;
    uptr gs;
    asm volatile("mov %%gs, %0" : "=r"(gs));

    SC(SYS_exit, ok && !(gs & 0xffff) ? 0 : 1, 0, 0);
}

static void exit_path_signals(void)
{
    int pid = self_pid();
    unsigned long before, after, set;

    struct aquila_sigaction sa = {(uptr) sse_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    check("signal: the SSE state kept across a handler", sse_kept(), 1);
    {
        int fc = sse_fork(), fst = -1;
        SC(SYS_waitpid, fc, &fst, 0);
        check("fork: the child has the parent's SSE state", fst, 0);
    }

    /* the handler's signal blocked while it runs: the second comes after */
    SC(SYS_sigmask, SIG_BLOCK, 0, &before);
    depth = max_depth = count = 0;
    sa = (struct aquila_sigaction) {(uptr) deep_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    check("signal: delivered twice", count, 2);
    check("signal: not nested (its own blocked)", max_depth, 1);
    SC(SYS_sigmask, SIG_BLOCK, 0, &after);
    check("signal: the mask restored after", after == before, 1);

    depth = max_depth = count = 0;
    sa.sa_flags = SA_NODEFER;
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    check("signal: SA_NODEFER nested", max_depth, 2);

    /* sa_mask: SIGUSR2 waits until the handler is done */
    got2 = got2_during = 0;
    sa = (struct aquila_sigaction) {(uptr) usr2_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR2, &sa, 0);
    sa = (struct aquila_sigaction) {(uptr) masking_handler, 1UL << SIGUSR2, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    check("signal: sa_mask blocks while the handler runs", got2_during, 0);
    check("signal: then delivered", got2, 1);

    /* another signal's handler runs in a handler */
    depth = nested = 0;
    sa = (struct aquila_sigaction) {(uptr) outer_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    check("signal: a handler in a handler", nested, 1);

    /* SA_RESETHAND: the default action afterwards */
    got_sig = 0;
    struct aquila_sigaction old;
    sa = (struct aquila_sigaction) {(uptr) handler, 0, SA_RESETHAND};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    SC(SYS_sigaction, SIGUSR1, 0, &old);
    check("signal: SA_RESETHAND ran", got_sig, SIGUSR1);
    check("signal: SA_RESETHAND reset", old.sa_handler, 0);

    /* sigsuspend: EINTR once the handler ran, the old mask back */
    sa = (struct aquila_sigaction) {(uptr) handler, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    set = 1UL << SIGUSR1;
    SC(SYS_sigmask, SIG_BLOCK, &set, &before);
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_timespec d = {0, 100000000};
        SC(SYS_nanosleep, &d, 0, 0);
        SC(SYS_kill, pid, SIGUSR1, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    got_sig = 0;
    unsigned long none = 0;
    check("signal: sigsuspend EINTR", SC(SYS_sigsuspend, &none, 0, 0), -EINTR);
    check("signal: sigsuspend's handler ran", got_sig, SIGUSR1);
    SC(SYS_sigmask, SIG_BLOCK, 0, &after);
    check("signal: sigsuspend's mask back", after, before | set);
    SC(SYS_sigmask, SIG_SETMASK, &before, 0);
    SC(SYS_waitpid, c, 0, 0);

    /* a read stopped and continued: made again, it gets the data */
    int p[2], st = -1;
    SC(SYS_pipe, p, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        char b = 0;
        int n = SC(SYS_read, p[0], &b, 1);
        SC(SYS_exit, n == 1 && b == 'z' ? 0 : 1, 0, 0);
    }
    aq_spin(20000000);
    SC(SYS_kill, c, SIGSTOP, 0);
    check("signal: stopped in a read", SC(SYS_waitpid, c, &st, WUNTRACED), c);
    SC(SYS_kill, c, SIGCONT, 0);
    SC(SYS_write, p[1], "z", 1);
    SC(SYS_waitpid, c, &st, 0);
    check("signal: the read made again after continuing", st, 0);
    SC(SYS_close, p[0], 0, 0);
    SC(SYS_close, p[1], 0, 0);

    /* sigreturn can't leave user mode, a frame nowhere kills */
    check("signal: forged sigreturn stays in user mode", aq_child_status(forged_sigreturn), 0);
    check("signal: sigreturn with no frame: SIGSEGV", aq_child_status(garbage_sigreturn) & 0x7f, SIGSEGV);
    check("signal: sigreturn past user space: SIGSEGV", aq_child_status(past_user_sigreturn) & 0x7f, SIGSEGV);
    struct aquila_sigaction past = {PAST_USER, 0, 0};
    check("signal: a handler past user space refused", SC(SYS_sigaction, SIGUSR1, &past, 0), -EFAULT);
    check("cpu: user mode's %gs, null, its own", aq_child_status(user_gs), 0);

    sa = (struct aquila_sigaction) {0, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    SC(SYS_sigaction, SIGUSR2, &sa, 0);
}

/*
 * User memory that isn't there: an address in user space with nothing
 * mapped (a page mapped, then unmapped). The calls that read or write it
 * fail with EFAULT and go on, what a read would have taken stays, a
 * buffer that runs into it is used up to it. In a child: a kernel that
 * kills the process for it (or worse) fails the last check.
 */
static void user_memory_child(void)
{
    /* two pages, the second one unmapped: `hole' is nothing, `edge' the
     * last 8 bytes before it */
    void *m = 0;
    struct aquila_mmap_args ma = {0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};

    if (SC(SYS_mmap, &ma, &m, 0) || SC(SYS_munmap, m, 2 * 4096, 0))
        SC(SYS_exit, 100, 0, 0);

    ma.len = 4096;
    ma.addr = m;
    ma.flags |= MAP_FIXED;

    if (SC(SYS_mmap, &ma, &m, 0))
        SC(SYS_exit, 101, 0, 0);

    char *hole = (char *) m + 4096;
    char *edge = hole - 8;
    char buf[64];

    /* a pipe: the bytes a faulting read would have taken stay */
    int p[2];
    SC(SYS_pipe, p, 0, 0);
    SC(SYS_write, p[1], "abcdefgh", 8);
    check("umem: read into nothing", SC(SYS_read, p[0], hole, 4), -EFAULT);
    check("umem: the bytes stay", SC(SYS_read, p[0], buf, 8), 8);
    check("umem: write from nothing", SC(SYS_write, p[1], hole, 4), -EFAULT);

    /* a buffer that runs into nothing: up to it */
    SC(SYS_write, p[1], "0123456789abcdef", 16);
    check("umem: read up to nothing", SC(SYS_read, p[0], edge, 16), 8);
    check("umem: what it read", edge[0] == '0' && edge[7] == '7', 1);
    check("umem: the rest stays", SC(SYS_read, p[0], buf, 16), 8);
    for (int i = 0; i < 8; ++i) edge[i] = 'A' + i;
    check("umem: write up to nothing", SC(SYS_write, p[1], edge, 16), 8);
    check("umem: what it wrote", SC(SYS_read, p[0], buf, 16) == 8 && buf[0] == 'A' && buf[7] == 'H', 1);
    SC(SYS_close, p[0], 0, 0);
    SC(SYS_close, p[1], 0, 0);

    /* files and devices */
    int fd = SC(SYS_open, "/init", O_RDONLY, 0);
    check("umem: file read into nothing", SC(SYS_read, fd, hole, 16), -EFAULT);
    check("umem: file read up to nothing", SC(SYS_read, fd, edge, 16), 8);
    SC(SYS_close, fd, 0, 0);
    SC(SYS_mknod, "/dev/zero", S_IFCHR | 0666, DEV(1, 5));
    int zero = SC(SYS_open, "/dev/zero", O_RDONLY, 0);
    check("umem: /dev/zero into nothing", SC(SYS_read, zero, hole, 16), -EFAULT);
    SC(SYS_close, zero, 0, 0);
    int fs = SC(SYS_open, "/tmp/umem", O_RDWR | O_CREAT, 0644);
    check("umem: tmpfs write from nothing", SC(SYS_write, fs, hole, 16), -EFAULT);
    check("umem: tmpfs write up to nothing", SC(SYS_write, fs, edge, 16), 8);
    struct aquila_stat st;
    check("umem: the file ends where the data does", SC(SYS_fstat, fs, &st, 0) == 0 && st.st_size == 8, 1);
    SC(SYS_close, fs, 0, 0);
    SC(SYS_unlink, "/tmp/umem", 0, 0);

    /* paths: nothing, and one that runs into nothing without its end */
    check("umem: open path nothing", SC(SYS_open, hole, O_RDONLY, 0), -EFAULT);
    for (int i = 0; i < 8; ++i) edge[i] = 'x';
    check("umem: open path into nothing", SC(SYS_open, edge, O_RDONLY, 0), -EFAULT);
    check("umem: stat into nothing", SC(SYS_stat, "/init", hole, 0), -EFAULT);
    check("umem: stat path nothing", SC(SYS_stat, hole, buf, 0), -EFAULT);
    check("umem: chdir nothing", SC(SYS_chdir, hole, 0, 0), -EFAULT);
    check("umem: unlink nothing", SC(SYS_unlink, hole, 0, 0), -EFAULT);

    /* structures in and out */
    check("umem: uname", SC(SYS_uname, hole, 0, 0), -EFAULT);
    check("umem: getcwd", SC(SYS_getcwd, hole, 100, 0), -EFAULT);
    check("umem: fstat", SC(SYS_fstat, aq_out, hole, 0), -EFAULT);
    check("umem: TCGETS", SC(SYS_ioctl, aq_out, TCGETS, hole), -EFAULT);
    check("umem: TCSETS", SC(SYS_ioctl, aq_out, TCSETS, hole), -EFAULT);
    check("umem: pipe", SC(SYS_pipe, hole, 0, 0), -EFAULT);
    check("umem: sigaction", SC(SYS_sigaction, SIGUSR2, hole, 0), -EFAULT);
    check("umem: sigaction old", SC(SYS_sigaction, SIGUSR2, 0, hole), -EFAULT);
    check("umem: nanosleep", SC(SYS_nanosleep, hole, 0, 0), -EFAULT);
    check("umem: clock_gettime", SC(SYS_clock_gettime, CLOCK_MONOTONIC, hole, 0), -EFAULT);
    check("umem: getrandom", SC(SYS_getrandom, hole, 16, 0), -EFAULT);
    check("umem: select", SC(SYS_select, hole, 0, 0), -EFAULT);
    check("umem: poll", SC(SYS_poll, hole, 1, 0), -EFAULT);
    check("umem: times", SC(SYS_times, hole, 0, 0), -EFAULT);

    /* a new thread's stack, the arguments of a program */
    struct aquila_thread_args ut = {(uptr) hole + 4096, (uptr) thr, 0, 0, 0};
    check("umem: thread stack", SC(SYS_thread_create, &ut, 0, 0), -EFAULT);
    struct aquila_thread_args kt = {(uptr) landing_stack + sizeof(landing_stack), KADDR, 0, 0, 0};
    check("thread: starting in the kernel", SC(SYS_thread_create, &kt, 0, 0), -EINVAL);
#ifdef __x86_64__
    kt.entry = 0x8000000000000000ul;
    check("thread: starting at an address not canonical", SC(SYS_thread_create, &kt, 0, 0), -EINVAL);
#endif
    char *argv[] = {"/init", hole, 0};
    check("umem: execve argument", SC(SYS_execve, "/init", argv, 0), -EFAULT);

    SC(SYS_exit, 0, 0, 0);
}

static void user_memory(void)
{
    check("umem: the process goes on", aq_child_status(user_memory_child), 0);
}


/*
 * Orphans: a process whose parent exits is init's (this program is pid
 * 1). A group the exit leaves orphaned (no member has a parent in another
 * group of its session) with a stopped member gets SIGHUP and SIGCONT; in
 * an orphaned group the terminal's stops don't stop, a background read is
 * EIO.
 */
static void orphan_hup(int sig) { SC(SYS_exit, 4, 0, 0); (void) sig; }

/* until the parent is gone (init is the parent then): 0, 1 if it isn't */
static int orphan_wait(void)
{
    for (int i = 0; i < 1000; ++i) {
        if (SC(SYS_getppid, 0, 0, 0) == 1)
            return 0;
        aq_spin(100000);
    }

    return 1;
}

/* a session on a pty, a job in another group of it whose parent exits:
 * the job tells it what it saw; exits with the failed step */
static void orphan_terminal(void)
{
    int n = -1;

    /* if the job stops, nothing continues it: the alarm ends this */
    struct aquila_itimer it = {3000000000ull, 0};
    SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);

    SC(SYS_setsid, 0, 0, 0);
    int master = SC(SYS_open, "/dev/ptmx", O_RDWR, 0);
    SC(SYS_ioctl, master, TIOCGPTN, &n);
    int slave = SC(SYS_open, pts_path(n), O_RDWR, 0);
    if (master < 0 || slave < 0) SC(SYS_exit, 1, 0, 0);

    int done[2];
    SC(SYS_pipe, done, 0, 0);

    int m = SC(SYS_fork, 0, 0, 0);
    if (!m) {
        int j = SC(SYS_fork, 0, 0, 0);
        if (!j) {
            char b, r[2];
            SC(SYS_setpgid, 0, 0, 0);
            r[0] = orphan_wait() ? 'P' : SC(SYS_read, slave, &b, 1) == -EIO ? 'E' : 'R';
            SC(SYS_kill, SC(SYS_getpid, 0, 0, 0), SIGTSTP, 0);
            r[1] = 'T';
            SC(SYS_write, done[1], r, 2);
            SC(SYS_exit, 0, 0, 0);
        }
        SC(SYS_exit, 0, 0, 0);
    }

    int st;
    char r[2] = {0};
    SC(SYS_waitpid, m, &st, 0);
    if (SC(SYS_read, done[0], r, 2) != 2) SC(SYS_exit, 2, 0, 0);
    if (r[0] != 'E') SC(SYS_exit, 3, 0, 0);
    if (r[1] != 'T') SC(SYS_exit, 4, 0, 0);
    SC(SYS_exit, 0, 0, 0);
}

static void orphans(void)
{
    int p[2], c, g = -1, st = -1;
    SC(SYS_pipe, p, 0, 0);

    /* a child's child, once the child exits */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        g = SC(SYS_fork, 0, 0, 0);
        if (!g)
            SC(SYS_exit, orphan_wait() ? 6 : 5, 0, 0);
        SC(SYS_write, p[1], &g, sizeof(g));
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_read, p[0], &g, sizeof(g));
    check("orphan: init waits for it", SC(SYS_waitpid, g, &st, 0), g);
    check("orphan: its parent was init", st >> 8, 5);

    /* a zombie left by its parent: init's to wait for */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        g = SC(SYS_fork, 0, 0, 0);
        if (!g)
            SC(SYS_exit, 3, 0, 0);
        SC(SYS_write, p[1], &g, sizeof(g));
        aq_spin(20000000);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_read, p[0], &g, sizeof(g));
    check("orphan: a zombie left is init's", SC(SYS_waitpid, g, &st, 0), g);
    check("orphan: the zombie's status", st >> 8, 3);

    /* a stopped job whose shell exits: SIGHUP, SIGCONT */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        SC(SYS_setsid, 0, 0, 0);
        g = SC(SYS_fork, 0, 0, 0);
        if (!g) {
            struct aquila_sigaction hsa = {(uptr) orphan_hup, 0, 0};
            SC(SYS_sigaction, SIGHUP, &hsa, 0);
            SC(SYS_setpgid, 0, 0, 0);
            SC(SYS_kill, SC(SYS_getpid, 0, 0, 0), SIGSTOP, 0);
            SC(SYS_exit, 1, 0, 0);
        }
        SC(SYS_write, p[1], &g, sizeof(g));
        SC(SYS_waitpid, g, &st, WUNTRACED);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_read, p[0], &g, sizeof(g));
    check("orphan: a stopped group's shell exits", SC(SYS_waitpid, g, &st, 0), g);
    check("orphan: SIGHUP and SIGCONT", st >> 8, 4);

    check("orphan: on a terminal", aq_child_status(orphan_terminal), 0);

    SC(SYS_close, p[0], 0, 0);
    SC(SYS_close, p[1], 0, 0);
}


/*
 * Open files: the descriptors dup() and fork() copied refer to the same
 * one, its offset and status flags (FD_CLOEXEC is each descriptor's); a
 * call using one keeps it open while it waits, whatever is closed
 */
/* lseek's whence, as POSIX numbers them */
#define SEEK_SET    0
#define SEEK_CUR    1

static int ofile_pipe[2];
static volatile int ofile_read = -100;
static char ofile_stack[8192] __attribute__((aligned(16)));

static void ofile_reader(void *u, void *a)
{
    char b[8];
    ofile_read = SC(SYS_read, ofile_pipe[0], b, sizeof(b));
    SC(SYS_thread_exit, 0, 0, 0);
    for (;;);
    (void) u; (void) a;
}

/*
 * Threads started while another uses the FPU: each adds up its series in
 * floating point, exactly (halves), with its own registers; a new thread
 * used to start with the FPU's owner's (no trap), and both were wrong
 */
static char fpu_stacks[4][8192] __attribute__((aligned(16)));
static volatile int fpu_next, fpu_right[4];

static void fpu_summer(void *u, void *a)
{
    int me = __atomic_fetch_add(&fpu_next, 1, __ATOMIC_SEQ_CST);
    volatile double sum = 0, step = me + 0.5;

    for (int i = 1; i <= 1000000; ++i)
        sum += step;

    fpu_right[me] = sum == step * 1000000;
    SC(SYS_thread_exit, 0, 0, 0);
    for (;;);
    (void) u; (void) a;
}

static void fpu_threads(void)
{
    int tids[4], right = 0;

    for (int i = 0; i < 4; ++i) {
        struct aquila_thread_args ta = {(uptr) &fpu_stacks[i][8192], (uptr) fpu_summer, 0, 0, 0};
        tids[i] = SC(SYS_thread_create, &ta, 0, 0);
    }

    for (int i = 0; i < 4; ++i)
        SC(SYS_thread_join, tids[i], 0, 0);

    for (int i = 0; i < 4; ++i)
        right += fpu_right[i];

    check("fpu: threads' sums, their own registers", right, 4);
}

/* each file of / (the initramfs): its stat's inode number is the one
 * readdir gave */
static void inode_numbers(void)
{
    struct dirent d;
    struct aquila_stat st;
    char path[MAXNAMELEN + 2];
    int fd = SC(SYS_open, "/", O_RDONLY, 0), n = 0, same = 0;

    while (SC(SYS_readdir, fd, &d, 0) > 0) {
        if (d.d_name[0] == '.' && (!d.d_name[1] || (d.d_name[1] == '.' && !d.d_name[2])))
            continue;

        path[0] = '/';

        for (int i = 0; (path[i + 1] = d.d_name[i]); ++i)
            ;

        /* a directory may be a mount point: stat has the mounted root */
        if (SC(SYS_lstat, path, &st, 0) || S_ISDIR(st.st_mode))
            continue;

        n++;
        same += st.st_ino == d.d_ino;
    }

    SC(SYS_close, fd, 0, 0);
    check("inode numbers: stat's are readdir's", n > 0 && same == n, 1);
}

static void open_files(void)
{
    int st;

    /* fork: the child's read moves the parent's offset */
    int fd = SC(SYS_open, "/init", O_RDONLY, 0);
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        char b[4];
        SC(SYS_exit, SC(SYS_read, fd, b, 4) == 4 ? 0 : 1, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    check("open file: fork shares the offset", SC(SYS_lseek, fd, 0, SEEK_CUR), 4);

    /* dup: the offset, the status flags; not FD_CLOEXEC */
    int d = SC(SYS_fcntl, fd, F_DUPFD, 0);
    SC(SYS_lseek, fd, 10, SEEK_SET);
    check("open file: dup shares the offset", SC(SYS_lseek, d, 0, SEEK_CUR), 10);
    SC(SYS_fcntl, d, F_SETFL, O_NONBLOCK);
    check("open file: dup shares status flags", (SC(SYS_fcntl, fd, F_GETFL, 0) & O_NONBLOCK) != 0, 1);
    SC(SYS_fcntl, d, F_SETFD, FD_CLOEXEC);
    check("open file: FD_CLOEXEC is the descriptor's", SC(SYS_fcntl, fd, F_GETFD, 0), 0);
    check("open file: close one", SC(SYS_close, d, 0, 0), 0);
    check("open file: the other still reads", SC(SYS_lseek, fd, 0, SEEK_CUR), 10);
    SC(SYS_close, fd, 0, 0);

    /* a read waiting in a thread: the descriptor closed meanwhile, the
     * read end stays open until the read returns */
    struct aquila_sigaction ign = {1 /* SIG_IGN */, 0, 0}, dfl = {0 /* SIG_DFL */, 0, 0};
    SC(SYS_sigaction, SIGPIPE, &ign, 0);
    SC(SYS_pipe, ofile_pipe, 0, 0);
    struct aquila_thread_args ut = {(uptr) &ofile_stack[8192], (uptr) ofile_reader, 0, 0, 0};
    int tid = SC(SYS_thread_create, &ut, 0, 0);
    aq_spin(20000000);
    check("open file: closed while read", SC(SYS_close, ofile_pipe[0], 0, 0), 0);
    check("open file: the reader is still there", SC(SYS_write, ofile_pipe[1], "data", 4), 4);
    SC(SYS_thread_join, tid, 0, 0);
    check("open file: the read got it", ofile_read, 4);
    check("open file: then no reader", SC(SYS_write, ofile_pipe[1], "x", 1), -EPIPE);
    SC(SYS_close, ofile_pipe[1], 0, 0);
    SC(SYS_sigaction, SIGPIPE, &dfl, 0);
}


/*
 * A sleep stopped and continued goes on until the deadline it had (the
 * call is restarted, it keeps its deadline): a child waits 400 ms, it's
 * stopped at about 100 ms and continued at about 300 ms, it wakes at
 * about 400 ms, not 600. Exits with how long it waited, in 10 ms.
 */
static int stop_pipe[2];

static void stopped_nanosleep(void)
{
    int t = now_ms();
    struct aquila_timespec d = {0, 400000000};
    SC(SYS_nanosleep, &d, 0, 0);
    SC(SYS_exit, (now_ms() - t) / 10, 0, 0);
}

static void stopped_poll(void)
{
    int t = now_ms();
    struct pollfd p = {stop_pipe[0], POLLIN, 0};
    SC(SYS_poll, &p, 1, 400);
    SC(SYS_exit, (now_ms() - t) / 10, 0, 0);
}

static void stopped_select(void)
{
    int t = now_ms();
    u32 rset[2] = {1u << stop_pipe[0], 0};
    struct { uint64_t sec; long usec; } tv = {0, 400000};  /* the kernel's timeval */
    struct aquila_select_args sel = {stop_pipe[0] + 1, rset, 0, 0, &tv};
    SC(SYS_select, &sel, 0, 0);
    SC(SYS_exit, (now_ms() - t) / 10, 0, 0);
}

/* fn in a child stopped from 100 to 300 ms: how long it waited (10 ms) */
static int stopped_wait(void (*fn)(void))
{
    struct aquila_timespec d = {0, 100000000};
    int st, c = SC(SYS_fork, 0, 0, 0);

    if (!c)
        fn();

    SC(SYS_nanosleep, &d, 0, 0);
    SC(SYS_kill, c, SIGSTOP, 0);
    SC(SYS_waitpid, c, &st, WUNTRACED);
    d.tv_nsec = 200000000;
    SC(SYS_nanosleep, &d, 0, 0);
    SC(SYS_kill, c, SIGCONT, 0);
    SC(SYS_waitpid, c, &st, 0);

    return st >> 8;
}

static void stopped_sleeps(void)
{
    SC(SYS_pipe, stop_pipe, 0, 0);

    int n = stopped_wait(stopped_nanosleep);
    check("stopped nanosleep: its deadline kept (x10 ms)", n >= 38 && n <= 50 ? 40 : n, 40);
    n = stopped_wait(stopped_poll);
    check("stopped poll: its deadline kept (x10 ms)", n >= 38 && n <= 50 ? 40 : n, 40);
    n = stopped_wait(stopped_select);
    check("stopped select: its deadline kept (x10 ms)", n >= 38 && n <= 50 ? 40 : n, 40);

    SC(SYS_close, stop_pipe[0], 0, 0);
    SC(SYS_close, stop_pipe[1], 0, 0);
}


/*
 * Paths walked vnode by vnode: ".." after a symbolic link is the parent
 * of where it went; a current directory, an open one, are the same
 * directory after a rename; ".." crosses mounts both ways; a directory
 * that isn't searchable stops a lookup; a removed current directory has
 * no path; a trailing slash wants a directory
 */
static char cwd_buf[256];

static const char *cwd_now(void)
{
    if (SC(SYS_getcwd, cwd_buf, sizeof(cwd_buf), 0))
        return "(error)";
    return cwd_buf;
}

/* in a child: chdir, rename what has it, the cwd follows; exits with the
 * failed step */
static void cwd_renamed(void)
{
    char sb[256];

    if (SC(SYS_mkdir, "/tmp/pr", 0755, 0) || SC(SYS_mkdir, "/tmp/pr/x", 0755, 0)) SC(SYS_exit, 1, 0, 0);
    if (SC(SYS_chdir, "/tmp/pr/x", 0, 0)) SC(SYS_exit, 2, 0, 0);
    if (SC5(SYS_renameat, AT_FDCWD, "/tmp/pr", AT_FDCWD, "/tmp/pr2", 0)) SC(SYS_exit, 3, 0, 0);
    if (!str_eq(cwd_now(), "/tmp/pr2/x")) SC(SYS_exit, 4, 0, 0);

    int fd = SC(SYS_open, "f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0) SC(SYS_exit, 5, 0, 0);
    SC(SYS_close, fd, 0, 0);
    if (SC(SYS_stat, "/tmp/pr2/x/f", sb, 0)) SC(SYS_exit, 6, 0, 0);

    /* removed: no path, nothing found in it */
    SC(SYS_unlink, "/tmp/pr2/x/f", 0, 0);
    if (SC(SYS_rmdir, "/tmp/pr2/x", 0, 0)) SC(SYS_exit, 7, 0, 0);
    if (SC(SYS_getcwd, cwd_buf, sizeof(cwd_buf), 0) != -ENOENT) SC(SYS_exit, 8, 0, 0);
    if (SC(SYS_stat, "f", sb, 0) != -ENOENT) SC(SYS_exit, 9, 0, 0);

    SC(SYS_rmdir, "/tmp/pr2", 0, 0);
    SC(SYS_exit, 0, 0, 0);
}

/* in a child: up through mounts with ..; exits with the failed step */
static void cwd_mounts(void)
{
    if (SC(SYS_chdir, "/dev", 0, 0) || SC(SYS_chdir, "..", 0, 0) || !str_eq(cwd_now(), "/"))
        SC(SYS_exit, 1, 0, 0);

    if (SC(SYS_mkdir, "/tmp/mx", 0755, 0) || aq_mount("tmpfs", "/tmp/mx", 0)) SC(SYS_exit, 2, 0, 0);
    if (SC(SYS_mkdir, "/tmp/mx/in", 0755, 0)) SC(SYS_exit, 3, 0, 0);
    if (SC(SYS_chdir, "/tmp/mx/in", 0, 0) || !str_eq(cwd_now(), "/tmp/mx/in")) SC(SYS_exit, 4, 0, 0);
    if (SC(SYS_chdir, "../..", 0, 0) || !str_eq(cwd_now(), "/tmp")) SC(SYS_exit, 5, 0, 0);

    /* a mount point is no entry to remove or rename */
    if (SC(SYS_rmdir, "/tmp/mx", 0, 0) != -EBUSY) SC(SYS_exit, 6, 0, 0);

    SC(SYS_exit, 0, 0, 0);
}

/* in a child, unprivileged: a directory it can't search; exits with the
 * failed step */
static void search_permission(void)
{
    char sb[256];

    if (SC(SYS_setuid, 1000, 0, 0)) SC(SYS_exit, 1, 0, 0);
    if (SC(SYS_stat, "/tmp/ps/f", sb, 0) != -EACCES) SC(SYS_exit, 2, 0, 0);
    if (SC(SYS_open, "/tmp/ps/f", O_RDONLY, 0) != -EACCES) SC(SYS_exit, 3, 0, 0);
    if (SC(SYS_chdir, "/tmp/ps", 0, 0) != -EACCES) SC(SYS_exit, 4, 0, 0);
    SC(SYS_exit, 0, 0, 0);
}

static void vnode_paths(void)
{
    char sb[256], sb2[256];

    /* .. after a symbolic link */
    SC(SYS_mkdir, "/tmp/pa", 0755, 0);
    SC(SYS_mkdir, "/tmp/pa/b", 0755, 0);
    SC(SYS_symlinkat, "/tmp/pa/b", AT_FDCWD, "/tmp/pl");
    SC(SYS_stat, "/tmp/pa", sb, 0);
    check("path: .. after a link is where it went's", SC(SYS_stat, "/tmp/pl/..", sb2, 0) == 0 &&
          ST(sb2)->st_ino == ST(sb)->st_ino, 1);

    /* an open directory renamed: still it */
    int dirfd = SC(SYS_open, "/tmp/pa/b", O_RDONLY | O_DIRECTORY, 0);
    SC5(SYS_renameat, AT_FDCWD, "/tmp/pa/b", AT_FDCWD, "/tmp/pa/c", 0);
    int fd = SC5(SYS_openat, dirfd, "f", O_CREAT | O_WRONLY, 0644, 0);
    check("path: openat in a renamed directory", fd >= 0 && SC(SYS_stat, "/tmp/pa/c/f", sb, 0) == 0, 1);
    SC(SYS_close, fd, 0, 0);
    SC(SYS_close, dirfd, 0, 0);

    /* a trailing slash wants a directory */
    check("path: a file with a trailing slash", SC(SYS_stat, "/init/", sb, 0), -ENOTDIR);
    check("path: a directory with one", SC(SYS_stat, "/tmp/pa/", sb, 0), 0);

    check("path: the cwd renamed", aq_child_status(cwd_renamed) >> 8, 0);
    check("path: .. through mounts", aq_child_status(cwd_mounts) >> 8, 0);

    SC(SYS_mkdir, "/tmp/ps", 0700, 0);
    fd = SC(SYS_open, "/tmp/ps/f", O_CREAT | O_WRONLY, 0644);
    SC(SYS_close, fd, 0, 0);
    check("path: search permission", aq_child_status(search_permission) >> 8, 0);
}

/* a child that sleeps until it's killed */
static void child_sleeps(void)
{
    struct aquila_timespec ts = {60, 0};
    SC(SYS_nanosleep, &ts, 0, 0);
}

/* a child that tries to mount as a user that isn't root: its exit status
 * is mount()'s error */
static void child_mounts_as_user(void)
{
    struct { char *dev, *opt; } args = {0, 0};

    if (SC(SYS_setuid, 1000, 0, 0))
        SC(SYS_exit, 1, 0, 0);

    SC(SYS_exit, -aq_mount("tmpfs", "/tmp", &args), 0, 0);
}

/*
 * waitpid() with a status pointer that can't be written fails with
 * EFAULT, and the event it would have reported (a stop) is reported to
 * the next call; mount() is root's only (EPERM).
 */
static void wait_and_mount(void)
{
    int c = SC(SYS_fork, 0, 0, 0);

    if (!c) {
        child_sleeps();
        SC(SYS_exit, 0, 0, 0);
    }

    SC(SYS_kill, c, SIGSTOP, 0);
    aq_spin(30000000);

    check("waitpid: a bad status pointer", SC(SYS_waitpid, c, KADDR, WUNTRACED), -EFAULT);

    int st = 0;
    check("waitpid: the stop, reported after all", SC(SYS_waitpid, c, &st, WUNTRACED) == c && (st & 0xff) == 0x7f, 1);

    SC(SYS_kill, c, SIGKILL, 0);
    SC(SYS_waitpid, c, &st, 0);

    check("mount: not root, EPERM", aq_child_status(child_mounts_as_user) >> 8, EPERM);
}

void aq_main(void)
{
    script_interpreter();
    exec_alone();
    aq_begin("syscalls");

    char buf[512];

    check("read kaddr", SC(SYS_read, aq_out, KADDR, 10), -EFAULT);
    check("write kaddr", SC(SYS_write, aq_out, KADDR, 10), -EFAULT);
    check("uname kaddr", SC(SYS_uname, KADDR, 0, 0), -EFAULT);
    check("getcwd kaddr", SC(SYS_getcwd, KADDR, 100, 0), -EFAULT);
    check("stat kbuf", SC(SYS_stat, "/init", KADDR, 0), -EFAULT);
    check("stat kpath", SC(SYS_stat, KADDR, buf, 0), -EFAULT);
    check("open kpath", SC(SYS_open, KADDR, 0, 0), -EFAULT);
    check("stat ok", SC(SYS_stat, "/init", buf, 0), 0);
    check("fstat kbuf", SC(SYS_fstat, aq_out, KADDR, 0), -EFAULT);
    check("ioctl TCGETS kaddr", SC(SYS_ioctl, aq_out, TCGETS, KADDR), -EFAULT);
    check("ioctl TCGETS ok", SC(SYS_ioctl, aq_out, TCGETS, buf), 0);
    check("ioctl TIOCGWINSZ kaddr", SC(SYS_ioctl, aq_out, TIOCGWINSZ, KADDR), -EFAULT);
    check("pipe kaddr", SC(SYS_pipe, KADDR, 0, 0), -EFAULT);
    int fds[2];
    check("pipe ok", SC(SYS_pipe, fds, 0, 0), 0);
    check("fcntl dup big", SC(SYS_fcntl, fds[0], F_DUPFD, 1000), -EINVAL);
    check("fcntl dup 10", SC(SYS_fcntl, fds[0], F_DUPFD, 10), 10);
    check("fcntl dup 10 again", SC(SYS_fcntl, fds[0], F_DUPFD, 10), 11);
    check("close 10", SC(SYS_close, 10, 0, 0), 0);
    check("close 10 again", SC(SYS_close, 10, 0, 0), -EBADF);
    check("close 11", SC(SYS_close, 11, 0, 0), 0);
    check("close 999", SC(SYS_close, 999, 0, 0), -EBADF);

    /* pipe EOF: child writes then exits, parent must see data then EOF */
    {
        int p[2];
        char rb[16];
        check("pipe2 ok", SC(SYS_pipe, p, 0, 0), 0);
        int pid = SC(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            SC(SYS_close, p[0], 0, 0);
            SC(SYS_write, p[1], "hello", 5);
            SC(SYS_exit, 0, 0, 0);
        }
        check("pipe parent close w", SC(SYS_close, p[1], 0, 0), 0);
        int total = 0, r;
        while ((r = SC(SYS_read, p[0], rb + total, 16 - total)) > 0)
            total += r;
        check("pipe read data", total, 5);
        check("pipe read eof", r, 0);
        int st;
        SC(SYS_waitpid, pid, &st, 0);
        check("pipe close r", SC(SYS_close, p[0], 0, 0), 0);

        /* EPIPE: write with no readers left, SIGPIPE ignored */
        struct aquila_sigaction ign = {1 /* SIG_IGN */, 0, 0}, dfl = {0 /* SIG_DFL */, 0, 0};
        SC(SYS_sigaction, SIGPIPE, &ign, 0);
        check("pipe3 ok", SC(SYS_pipe, p, 0, 0), 0);
        check("pipe3 close r", SC(SYS_close, p[0], 0, 0), 0);
        check("pipe3 write epipe", SC(SYS_write, p[1], "x", 1), -EPIPE);
        SC(SYS_sigaction, SIGPIPE, &dfl, 0);

        /* by default SIGPIPE kills the writer */
        int pc = SC(SYS_fork, 0, 0, 0);
        if (!pc) {
            SC(SYS_write, p[1], "x", 1);
            SC(SYS_exit, 0, 0, 0);
        }
        int pst = 0;
        SC(SYS_waitpid, pc, &pst, 0);
        check("pipe3 SIGPIPE", pst & 0x7f, SIGPIPE);
        check("pipe3 close w", SC(SYS_close, p[1], 0, 0), 0);
    }

    /* two writers waiting for room in a full pipe: they wait, and don't
     * wake each other up again and again (the pipe's readers and writers
     * share a wait queue), which kept the kernel busy for good */
    {
        static char big[65536];
        int p[2], w[2];
        SC(SYS_pipe, p, 0, 0);

        for (int i = 0; i < 2; ++i) {
            if (!(w[i] = SC(SYS_fork, 0, 0, 0))) {
                SC(SYS_close, p[0], 0, 0);
                SC(SYS_exit, SC(SYS_write, p[1], big, sizeof(big)) == sizeof(big) ? 0 : 1, 0, 0);
            }
        }

        SC(SYS_close, p[1], 0, 0);
        struct aquila_timespec ts = {0, 200000000};
        check("pipe: two writers waiting for room, the others run", SC(SYS_nanosleep, &ts, 0, 0), 0);

        long total = 0, r;
        while ((r = SC(SYS_read, p[0], big, sizeof(big))) > 0)
            total += r;
        check("pipe: then all both wrote", total, 2 * sizeof(big));

        int st0 = -1, st1 = -1;
        SC(SYS_waitpid, w[0], &st0, 0);
        SC(SYS_waitpid, w[1], &st1, 0);
        check("pipe: both writers done", st0 | st1, 0);
        SC(SYS_close, p[0], 0, 0);
    }

    /* a write larger than a pipe holds, with the reader asleep */
    int bp[2];
    SC(SYS_pipe, bp, 0, 0);
    int bc = SC(SYS_fork, 0, 0, 0);
    if (!bc) {
        int total = 0, n;
        SC(SYS_close, bp[1], 0, 0);
        while ((n = SC(SYS_read, bp[0], buf, sizeof(buf))) > 0)
            total += n;
        SC(SYS_exit, total == 3000 ? 0 : 1, 0, 0);
    }
    SC(SYS_close, bp[0], 0, 0);
    aq_spin(10000000);  /* let the reader block first */
    static char big[3000];
    check("big pipe write", SC(SYS_write, bp[1], big, sizeof(big)), 3000);
    SC(SYS_close, bp[1], 0, 0);
    int bst = 0;
    SC(SYS_waitpid, bc, &bst, 0);
    check("reader got it all", bst >> 8, 0);

    /* ping-pong through two pipes: a thread that goes to sleep lets the
     * other run right away, not at the next timer tick (2 ms, 800 ms for
     * these 200 round trips) */
    int ping[2], pong[2];
    SC(SYS_pipe, ping, 0, 0);
    SC(SYS_pipe, pong, 0, 0);
    int ppc = SC(SYS_fork, 0, 0, 0);
    if (!ppc) {
        char c;
        SC(SYS_close, ping[1], 0, 0);
        SC(SYS_close, pong[0], 0, 0);
        while (SC(SYS_read, ping[0], &c, 1) == 1)
            SC(SYS_write, pong[1], &c, 1);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_close, ping[0], 0, 0);
    SC(SYS_close, pong[1], 0, 0);
    int pp0 = now_ms(), trips = 0;
    for (int i = 0; i < 200; ++i) {
        char c = 'x';
        SC(SYS_write, ping[1], &c, 1);
        trips += SC(SYS_read, pong[0], &c, 1) == 1;
    }
    int took = now_ms() - pp0;
    check("pipe ping-pong", trips, 200);
    check("pipe ping-pong: not a tick each", took < 100, 1);
    SC(SYS_close, ping[1], 0, 0);
    SC(SYS_close, pong[0], 0, 0);
    SC(SYS_waitpid, ppc, 0, 0);

    check("read badfd", SC(SYS_read, 50, buf, 1), -EBADF);

    /* signals */
    int pid = SC(SYS_getpid, 0, 0, 0);
    struct aquila_sigaction sa = {1 /* SIG_IGN */, 0, 0};
    check("sigaction ign", SC(SYS_sigaction, SIGUSR1, &sa, 0), 0);
    check("kill self ign", SC(SYS_kill, pid, SIGUSR1, 0), 0);
    check("kill self 0", SC(SYS_kill, pid, 0, 0), 0);
    check("kill self 99", SC(SYS_kill, pid, 99, 0), -EINVAL);
    check("kill nopid", SC(SYS_kill, 3999, SIGTERM, 0), -ESRCH);
    check("kill winch dfl", SC(SYS_kill, pid, SIGWINCH, 0), 0);
    check("sigaction kact", SC(SYS_sigaction, SIGUSR2, KADDR, 0), -EFAULT);
    sa.sa_handler = KADDR;
    check("sigaction khandler", SC(SYS_sigaction, SIGUSR2, &sa, 0), -EFAULT);
    sa.sa_handler = (uptr) handler;
    check("sigaction handler", SC(SYS_sigaction, SIGUSR2, &sa, 0), 0);
    check("kill self handler", SC(SYS_kill, pid, SIGUSR2, 0), 0);
    check("handler ran", got_sig, SIGUSR2);

    /* waitpid */
    int st = 0;
    int c = SC(SYS_fork, 0, 0, 0);
    if (!c) SC(SYS_exit, 3, 0, 0);
    check("waitpid NULL", SC(SYS_waitpid, c, 0, 0), c);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) { aq_spin(30000000); SC(SYS_exit, 4, 0, 0); }
    check("waitpid WNOHANG", SC(SYS_waitpid, c, 0, 1), 0);
    check("waitpid kstat", SC(SYS_waitpid, c, KADDR, 0), -EFAULT);
    check("waitpid pgrp", SC(SYS_waitpid, -12345, 0, 1), -ECHILD);
    check("waitpid st", SC(SYS_waitpid, c, &st, 0), c);
    check("status", st, 4 << 8);

    /* the pid of a process just reaped isn't the next one's */
    int next = SC(SYS_fork, 0, 0, 0);
    if (!next) SC(SYS_exit, 0, 0, 0);
    SC(SYS_waitpid, next, 0, 0);
    check("a reaped pid isn't given again at once", next != c, 1);

    /* kill a zombie */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) SC(SYS_exit, 5, 0, 0);
    aq_spin(20000000);
    check("kill zombie", SC(SYS_kill, c, SIGTERM, 0), 0);
    check("reap zombie", SC(SYS_waitpid, c, &st, 0), c);

    /* async signal to busy child (delivered on context switch) */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_sigaction s2 = {(uptr) child_handler, 0, 0};
        SC(SYS_sigaction, SIGUSR1, &s2, 0);
        for (;;);
    }
    aq_spin(30000000);
    check("kill busy child", SC(SYS_kill, c, SIGUSR1, 0), 0);
    check("busy child wait", SC(SYS_waitpid, c, &st, 0), c);
    check("busy child status", st, 7 << 8);

    /* sbrk / munmap */
    int brk = SC(SYS_sbrk, 0, 0, 0);
    check("sbrk neg", SC(SYS_sbrk, -0x10000000, 0, 0), -1);
    /* more than there is below the stack */
#ifdef __x86_64__
    check("sbrk huge", SC(SYS_sbrk, 0x800000000000ul, 0, 0), -1);
#else
    check("sbrk huge", SC(SYS_sbrk, 0xB0000000, 0, 0), -1);
#endif
    check("sbrk ok", SC(SYS_sbrk, 0x1000, 0, 0), brk);
    check("munmap len0", SC(SYS_munmap, brk & ~0xfff, 0, 0), -EINVAL);

    /* select */
    u32 rset[2] = {~0u, ~0u};
    struct aquila_select_args sel = {100000, rset, 0, 0, 0};
    /* more than FD_SETSIZE */
    check("select huge nfds", SC(SYS_select, &sel, 0, 0), -EINVAL);
    check("select kargs", SC(SYS_select, KADDR, 0, 0), -EFAULT);

    /* mmap */
    void *mret;
    struct aquila_mmap_args ma = {(void *) KADDR, 0x1000, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE, aq_out, 0};
    check("mmap fixed kernel", SC(SYS_mmap, &ma, &mret, 0), -EINVAL);
    check("mmap kret", SC(SYS_mmap, &ma, KADDR, 0), -EFAULT);

    /* anonymous memory: zero-filled pages, private to the process */
    struct aquila_mmap_args aa = {0, 100, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
    check("mmap anonymous", SC(SYS_mmap, &aa, &mret, 0), 0);
    volatile char *anon = mret;
    check("mmap anonymous page aligned", (uptr) anon & 0xfff, 0);
    check("mmap anonymous zero", anon[0] | anon[99] | anon[4095], 0);
    anon[0] = 'p';
    int ac = SC(SYS_fork, 0, 0, 0);
    if (!ac) {
        int ok = anon[0] == 'p';
        anon[0] = 'c';
        SC(SYS_exit, ok, 0, 0);
    }
    int ast = 0;
    SC(SYS_waitpid, ac, &ast, 0);
    check("mmap anonymous copied by fork", ast >> 8, 1);
    check("mmap anonymous private", anon[0], 'p');
    check("munmap anonymous", SC(SYS_munmap, anon, 100, 0), 0);
    check("munmap again", SC(SYS_munmap, anon, 100, 0), -EINVAL);
    aa.flags = MAP_SHARED | MAP_ANONYMOUS;
    check("mmap anonymous shared", SC(SYS_mmap, &aa, &mret, 0), -EINVAL);
    aa.flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;
    aa.addr = (void *) 0x40000100;
    check("mmap fixed unaligned", SC(SYS_mmap, &aa, &mret, 0), -EINVAL);

#ifdef __x86_64__
    /* a large mapping with one page in it: fork() and munmap() go over
     * its page tables, not its 4 billion pages */
    {
        struct aquila_mmap_args large = {0, 1UL << 44, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0};
        check("mmap 16 TiB", SC(SYS_mmap, &large, &mret, 0), 0);
        int start = now_ms();
        ((volatile char *) mret)[12345] = 1;
        int lc = SC(SYS_fork, 0, 0, 0);
        if (!lc)
            SC(SYS_exit, 0, 0, 0);
        SC(SYS_waitpid, lc, &ast, 0);
        check("munmap 16 TiB", SC(SYS_munmap, mret, 1UL << 44, 0), 0);
        check("fork and munmap of 16 TiB with a page in it: quick", now_ms() - start < 1000, 1);
    }
#endif

    /* threads */
    struct aquila_thread_args ut = {KADDR, (uptr) thr, 0, 0, 0};
    check("thread kstack", SC(SYS_thread_create, &ut, 0, 0), -EFAULT);
    ut.stack = (uptr) &tstack[8192];
    int tid = SC(SYS_thread_create, &ut, 0, 0);
    void *tval = 0;
    check("thread join", SC(SYS_thread_join, tid, &tval, 0), tid);
    check("thread join value", (int) (long) tval, 7);
    check("thread ran", thread_ran, 1);
    check("thread_self in the thread", thread_self, tid);
    check("thread_self", SC(SYS_thread_self, 0, 0, 0) != tid && SC(SYS_thread_self, 0, 0, 0) > 0, 1);
    check("thread join again", SC(SYS_thread_join, tid, 0, 0), -ESRCH);
    check("thread join itself", SC(SYS_thread_join, SC(SYS_thread_self, 0, 0, 0), 0, 0), -EDEADLK);

    /* threads made and joined, over and over: each joined one is freed,
     * and none gets an id another one has had */
    int tids_ok = 1, last = tid;
    for (int i = 0; i < 50; ++i) {
        int t = SC(SYS_thread_create, &ut, 0, 0);
        tids_ok = tids_ok && t > last && SC(SYS_thread_join, t, 0, 0) == t;
        last = t;
    }
    check("threads made and joined, new ids", tids_ok, 1);

    /* sockets */
    int s = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sun = {AF_UNIX, "/init"};
    check("connect nonsock", SC(SYS_connect, s, &sun, sizeof(sun)), -ECONNREFUSED);
    check("connect kaddr", SC(SYS_connect, s, KADDR, 4), -EFAULT);
    check("listen unbound", SC(SYS_listen, s, 5, 0), -EINVAL);
    check("fstat sock", SC(SYS_fstat, s, buf, 0), 0);
    check("lseek sock", SC(SYS_lseek, s, 0, 0), -ESPIPE);
    check("close sock", SC(SYS_close, s, 0, 0), 0);
    check("unix datagram socket", SC(SYS_socket, AF_UNIX, SOCK_DGRAM, 0), -EPROTONOSUPPORT);

    /* closing a listener refuses a blocked connect() */
    {
        check("mount tmpfs", aq_mount("tmpfs", "/tmp", 0), 0);
        struct sockaddr_un la = {AF_UNIX, "/tmp/sock"};
        int ls = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        check("bind", SC(SYS_bind, ls, &la, sizeof(la)), 0);
        check("listen", SC(SYS_listen, ls, 1, 0), 0);
        int cpid = SC(SYS_fork, 0, 0, 0);
        if (cpid == 0) {
            SC(SYS_close, ls, 0, 0);  /* drop our inherited copy of the listener */
            int cs = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
            int r = SC(SYS_connect, cs, &la, sizeof(la));
            SC(SYS_exit, -r, 0, 0);
        }
        aq_spin(50000000);  /* let the child block in connect() */
        check("close listener", SC(SYS_close, ls, 0, 0), 0);
        int st = 0;
        check("refused child wait", SC(SYS_waitpid, cpid, &st, 0), cpid);
        check("refused connect", (st >> 8) & 0xff, ECONNREFUSED);
        int ls2 = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        check("connect after close", SC(SYS_connect, ls2, &la, sizeof(la)), -ECONNREFUSED);
        SC(SYS_close, ls2, 0, 0);
    }

    /* a file's mapping sees what is written to the file, and zeros past
     * its end once it's truncated (its pages in the page cache follow) */
    int mf = SC(SYS_open, "/tmp/mapped", O_CREAT | O_RDWR, 0644);
    SC(SYS_write, mf, "old!", 4);
    struct aquila_mmap_args fa = {0, 4096, PROT_READ, MAP_PRIVATE, mf, 0};
    check("mmap file", SC(SYS_mmap, &fa, &mret, 0), 0);
    volatile char *mapped = mret;
    check("mmap file read", mapped[0], 'o');
    SC(SYS_lseek, mf, 0, 0);
    SC(SYS_write, mf, "new!", 4);
    check("mmap file sees a write", mapped[0], 'n');
    check("ftruncate mapped", SC(SYS_ftruncate, mf, 0, 0), 0);
    check("mmap file zero after truncate", mapped[0], 0);
    SC(SYS_munmap, mapped, 4096, 0);
    SC(SYS_close, mf, 0, 0);
    SC(SYS_unlink, "/tmp/mapped", 0, 0);

    /* O_APPEND: every write at the end, wherever the offset was moved */
    char ab[8] = {0};
    int af = SC(SYS_open, "/tmp/append", O_CREAT | O_RDWR | O_APPEND, 0644);
    SC(SYS_write, af, "ab", 2);
    SC(SYS_lseek, af, 0, SEEK_SET);
    check("O_APPEND write", SC(SYS_write, af, "cd", 2), 2);
    check("O_APPEND: the offset is the end", SC(SYS_lseek, af, 0, SEEK_CUR), 4);
    SC(SYS_fcntl, af, F_SETFL, 0);
    SC(SYS_lseek, af, 0, SEEK_SET);
    SC(SYS_write, af, "A", 1);
    SC(SYS_fcntl, af, F_SETFL, O_APPEND);
    SC(SYS_lseek, af, 0, SEEK_SET);
    SC(SYS_write, af, "e", 1);
    SC(SYS_lseek, af, 0, SEEK_SET);
    SC(SYS_read, af, ab, 5);
    check("O_APPEND, on and off with F_SETFL", ab[0] == 'A' && ab[1] == 'b' && ab[2] == 'c' &&
          ab[3] == 'd' && ab[4] == 'e', 1);
    SC(SYS_close, af, 0, 0);
    SC(SYS_unlink, "/tmp/append", 0, 0);

#if __SIZEOF_LONG__ == 8
    /* an offset past 2 GiB (off_t is 64 bits on x86_64 and aarch64): all
     * of it comes back */
    int bf = SC(SYS_open, "/tmp/far", O_CREAT | O_RDWR, 0644);
    long far = 3L << 30;
    check("lseek past 2 GiB", SC(SYS_lseek, bf, far, SEEK_SET) == far, 1);
    check("lseek past 2 GiB: SEEK_CUR", SC(SYS_lseek, bf, 1, SEEK_CUR) == far + 1, 1);
    SC(SYS_close, bf, 0, 0);
    SC(SYS_unlink, "/tmp/far", 0, 0);
#endif

    /* shutdown(): one direction ends, the other still works */
    {
        struct sockaddr_un la = {AF_UNIX, "/tmp/shut"};
        int ls = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        SC(SYS_bind, ls, &la, sizeof(la));
        SC(SYS_listen, ls, 1, 0);
        int taken = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
        check("bind a path taken", SC(SYS_bind, taken, &la, sizeof(la)), -EADDRINUSE);
        SC(SYS_close, taken, 0, 0);
        check("shutdown listening", SC(SYS_shutdown, ls, SHUT_RDWR, 0), -ENOTCONN);
        check("shutdown not a socket", SC(SYS_shutdown, aq_out, SHUT_WR, 0), -ENOTSOCK);
        check("shutdown bad fd", SC(SYS_shutdown, 99, SHUT_WR, 0), -EBADF);

        /* the open flags' top bit is the kernel's own "socket" flag: a
         * file opened with it is still a file */
        int nf = SC(SYS_open, "/init", O_RDONLY | 0x80000000, 0);
        check("open with flags of its own: not a socket", SC(SYS_accept, nf, 0, 0), -ENOTSOCK);
        SC(SYS_close, nf, 0, 0);
        int cpid = SC(SYS_fork, 0, 0, 0);
        if (cpid == 0) {
            int cs = SC(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
            char c = 0;
            if (SC(SYS_connect, cs, &la, sizeof(la))) SC(SYS_exit, 1, 0, 0);
            /* the end of what the server sends, then an answer */
            if (SC(SYS_read, cs, &c, 1) != 0) SC(SYS_exit, 2, 0, 0);
            SC(SYS_write, cs, "a", 1);
            SC(SYS_exit, 0, 0, 0);
        }
        int conn = SC(SYS_accept, ls, 0, 0);

        /* FIONBIO, on a socket as on anything */
        int on = 1;
        check("FIONBIO", SC(SYS_ioctl, conn, FIONBIO, &on), 0);
        check("FIONBIO sets O_NONBLOCK", (SC(SYS_fcntl, conn, F_GETFL, 0) & O_NONBLOCK) != 0, 1);
        on = 0;
        SC(SYS_ioctl, conn, FIONBIO, &on);
        check("FIONBIO clears O_NONBLOCK", SC(SYS_fcntl, conn, F_GETFL, 0) & O_NONBLOCK, 0);
        check("FIONBIO kaddr", SC(SYS_ioctl, conn, FIONBIO, KADDR), -EFAULT);

        /* non-blocking: nothing to read, no connection to accept */
        char nb;
        SC(SYS_fcntl, conn, F_SETFL, O_NONBLOCK);
        check("recv nonblocking empty", SC(SYS_read, conn, &nb, 1), -EAGAIN);
        SC(SYS_fcntl, conn, F_SETFL, 0);
        SC(SYS_fcntl, ls, F_SETFL, O_NONBLOCK);
        check("accept nonblocking none", SC(SYS_accept, ls, 0, 0), -EAGAIN);
        SC(SYS_fcntl, ls, F_SETFL, 0);

        check("shutdown bad how", SC(SYS_shutdown, conn, 0, 0), -EINVAL);
        check("shutdown SHUT_WR", SC(SYS_shutdown, conn, SHUT_WR, 0), 0);
        char c = 0;
        check("read after shutdown SHUT_WR", SC(SYS_read, conn, &c, 1), 1);
        check("read the answer", c, 'a');
        int st = 0;
        SC(SYS_waitpid, cpid, &st, 0);
        check("peer read the end", st >> 8, 0);
        SC(SYS_close, conn, 0, 0);
        SC(SYS_close, ls, 0, 0);
    }

    /* execve */
    char *bad_argv[] = {"x", (char *) KADDR, 0};
    check("execve kargv", SC(SYS_execve, "/init", bad_argv, 0), -EFAULT);
    check("execve kpath", SC(SYS_execve, KADDR, 0, 0), -EFAULT);
    check("execve noent", SC(SYS_execve, "/nonexistent", 0, 0), -ENOENT);

    /* bad ELF loading into kernel space (/tmp is tmpfs, mounted above) */
    unsigned char *e = bad_elf;
    e[0] = 0x7f; e[1] = 'E'; e[2] = 'L'; e[3] = 'F'; e[4] = 1; e[5] = 1; e[6] = 1;
    *(unsigned short *)(e + 16) = 2; *(unsigned short *)(e + 18) = 3; *(u32 *)(e + 20) = 1;
    *(u32 *)(e + 24) = 0x08048000; *(u32 *)(e + 28) = 52; *(unsigned short *)(e + 40) = 52;
    *(unsigned short *)(e + 42) = 32; *(unsigned short *)(e + 44) = 1;
    u32 *ph = (u32 *)(e + 52);
    ph[0] = 1; ph[1] = 0; ph[2] = 0xBFFFF000; ph[3] = 0xBFFFF000; ph[4] = 0x10; ph[5] = 0x10000; ph[6] = 6; ph[7] = 0x1000;
    int fd = SC(SYS_open, "/tmp/bad", O_CREAT | O_RDWR, 0777);
    check("write bad elf", SC(SYS_write, fd, bad_elf, sizeof(bad_elf)), sizeof(bad_elf));
    SC(SYS_close, fd, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) SC(SYS_exit, SC(SYS_execve, "/tmp/bad", 0, 0) == -ENOEXEC ? 98 : 99, 0, 0);
    check("bad elf wait", SC(SYS_waitpid, c, &st, 0), c);
#ifdef __x86_64__
    /* an i386 program: refused before the process's image is replaced */
    check("i386 elf refused", st, 98 << 8);
#else
    check("bad elf status", st, (SIGSEGV << 8) | SIGSEGV);
#endif

    /* execute permission */
    SC(SYS_chmod, "/tmp/bad", 0644, 0);
    check("execve without execute permission", SC(SYS_execve, "/tmp/bad", 0, 0), -EACCES);
    check("execve directory", SC(SYS_execve, "/tmp", 0, 0), -EACCES);

    /* directories are read with readdir, not read */
    const char *dirs[] = {"/", "/tmp", "/dev"};
    for (int i = 0; i < 3; ++i) {
        char dbuf[16];
        int dfd = SC(SYS_open, dirs[i], O_RDONLY, 0);
        check("read directory", SC(SYS_read, dfd, dbuf, sizeof(dbuf)), -EISDIR);
        SC(SYS_close, dfd, 0, 0);
    }
    check("open directory for writing", SC(SYS_open, "/tmp", O_WRONLY, 0), -EISDIR);

    /* scripts: "#!interpreter argument", this program is the interpreter */
    fd = SC(SYS_open, "/tmp/script", O_CREAT | O_RDWR, 0755);
    SC(SYS_write, fd, "#! /init -x y \nignored\n", 23);
    SC(SYS_close, fd, 0, 0);
    char *script_argv[] = {"script", "arg", 0};
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) { SC(SYS_execve, "/tmp/script", script_argv, 0); SC(SYS_exit, 99, 0, 0); }
    SC(SYS_waitpid, c, &st, 0);
    check("execve script", st >> 8, 42);
    SC(SYS_chmod, "/tmp/script", 0644, 0);
    check("execve script without execute permission", SC(SYS_execve, "/tmp/script", script_argv, 0), -EACCES);
    fd = SC(SYS_open, "/tmp/loop", O_CREAT | O_RDWR, 0755);
    SC(SYS_write, fd, "#!/tmp/loop\n", 12);
    SC(SYS_close, fd, 0, 0);
    check("execve script loop", SC(SYS_execve, "/tmp/loop", script_argv, 0), -ELOOP);
    fd = SC(SYS_open, "/tmp/noint", O_CREAT | O_RDWR, 0755);
    SC(SYS_write, fd, "#!  \n", 5);
    SC(SYS_close, fd, 0, 0);
    check("execve script without interpreter", SC(SYS_execve, "/tmp/noint", script_argv, 0), -ENOEXEC);

    /* user ids: the removed auth syscall, setuid/setgid rules */
    check("auth syscall removed", SC(35, 0, 0, 0), -ENOSYS);

    /* undefined calls in a loop: each fails, and the console isn't
     * flooded (a few messages a second, the rest counted) */
    {
        int start = now_ms(), enosys = 0;

        for (int i = 0; i < 20000; i++)
            enosys += SC(9999, 0, 0, 0) == -ENOSYS;

        check("undefined syscall 20000 times: ENOSYS", enosys, 20000);
        check("undefined syscall 20000 times: quick", now_ms() - start < 1000, 1);
    }

    int fd2 = SC(SYS_open, "/tmp/secret", O_CREAT | O_RDWR, 0600);
    check("create root-only file", fd2 >= 0, 1);
    SC(SYS_close, fd2, 0, 0);

    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        /* exit with the number of the first failed step */
        if (SC(SYS_setgid, 1000, 0, 0) != 0) SC(SYS_exit, 1, 0, 0);
        if (SC(SYS_setuid, 1000, 0, 0) != 0) SC(SYS_exit, 2, 0, 0);
        if (SC(SYS_getuid, 0, 0, 0) != 1000) SC(SYS_exit, 3, 0, 0);
        if (SC(SYS_getgid, 0, 0, 0) != 1000) SC(SYS_exit, 4, 0, 0);
        if (SC(SYS_setuid, 0, 0, 0) != -EPERM) SC(SYS_exit, 5, 0, 0);
        if (SC(SYS_setgid, 0, 0, 0) != -EPERM) SC(SYS_exit, 6, 0, 0);
        if (SC(SYS_setuid, 1000, 0, 0) != 0) SC(SYS_exit, 7, 0, 0);
        if (SC(SYS_open, "/tmp/secret", O_RDONLY, 0) != -EACCES) SC(SYS_exit, 8, 0, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    check("setuid wait", SC(SYS_waitpid, c, &st, 0), c);
    check("setuid/setgid rules (failed step)", st >> 8, 0);
    check("root still root", SC(SYS_getuid, 0, 0, 0), 0);

    /* --- descriptor flags (fcntl) --- */
    fd = SC(SYS_open, "/tmp/flags", O_CREAT | O_WRONLY, 0644);
    check("F_GETFD initially", SC(SYS_fcntl, fd, F_GETFD, 0), 0);
    check("F_SETFD FD_CLOEXEC", SC(SYS_fcntl, fd, F_SETFD, FD_CLOEXEC), 0);
    check("F_GETFD after set", SC(SYS_fcntl, fd, F_GETFD, 0), FD_CLOEXEC);
    /* FD_CLOEXEC == O_WRONLY used to overwrite the open flags */
    check("F_GETFL unchanged", SC(SYS_fcntl, fd, F_GETFL, 0) & O_ACCMODE, O_WRONLY);
    check("still writable", SC(SYS_write, fd, "0123456789", 10), 10);
    int dfd = SC(SYS_fcntl, fd, F_DUPFD_CLOEXEC, 20);
    check("F_DUPFD_CLOEXEC", dfd >= 20, 1);
    check("dup has FD_CLOEXEC", SC(SYS_fcntl, dfd, F_GETFD, 0), FD_CLOEXEC);
    SC(SYS_close, dfd, 0, 0);
    int cfd = SC(SYS_open, "/tmp/flags", O_RDONLY | O_CLOEXEC, 0);
    check("O_CLOEXEC", SC(SYS_fcntl, cfd, F_GETFD, 0), FD_CLOEXEC);

    /* --- ftruncate --- */
    check("ftruncate", SC(SYS_ftruncate, fd, 4, 0), 0);
    char st_buf[128];
    SC(SYS_fstat, fd, st_buf, 0);
    check("size after ftruncate", ((struct aquila_stat *) st_buf)->st_size, 4);
    check("ftruncate read-only fd", SC(SYS_ftruncate, cfd, 0, 0), -EINVAL);
    check("ftruncate negative", SC(SYS_ftruncate, fd, -1, 0), -EINVAL);
    SC(SYS_close, cfd, 0, 0);
    SC(SYS_close, fd, 0, 0);

    /* --- symbolic links (tests/syscalls/rootfs) --- */
    fd = SC(SYS_open, "/lnk/relative", O_RDONLY, 0);
    check("relative symlink", fd >= 0, 1);
    char lbuf[16] = {0};
    check("read through symlink", SC(SYS_read, fd, lbuf, 11), 11);
    SC(SYS_close, fd, 0, 0);
    fd = SC(SYS_open, "/lnk/absolute", O_RDONLY, 0);
    check("absolute symlink", fd >= 0, 1);
    SC(SYS_close, fd, 0, 0);
    check("symlink loop", SC(SYS_open, "/lnk/loop", O_RDONLY, 0), -ELOOP);

    /* --- files (on the tmpfs mounted at /tmp) --- */
    char sb[128];
    check("umask", SC(SYS_umask, 022, 0, 0), 022);
    fd = SC(SYS_open, "/tmp/a", O_CREAT | O_WRONLY, 0666);
    check("create", fd >= 0, 1);
    SC(SYS_write, fd, "hello", 5);
    SC(SYS_fstat, fd, sb, 0);
    check("umask applied", ST(sb)->st_mode & 07777, 0644);
    SC(SYS_close, fd, 0, 0);
    check("O_EXCL existing", SC(SYS_open, "/tmp/a", O_CREAT | O_EXCL | O_WRONLY, 0644), -EEXIST);
    check("O_DIRECTORY on a file", SC(SYS_open, "/tmp/a", O_RDONLY | O_DIRECTORY, 0), -ENOTDIR);

    check("mkdir", SC(SYS_mkdir, "/tmp/d", 0755, 0), 0);
    check("mkdir existing", SC(SYS_mkdir, "/tmp/d", 0755, 0), -EEXIST);
    check("mkdir mountpoint", SC(SYS_mkdir, "/tmp", 0755, 0), -EEXIST);
    check("open directory for writing", SC(SYS_open, "/tmp/d", O_WRONLY, 0), -EISDIR);
    int dirfd = SC(SYS_open, "/tmp/d", O_RDONLY | O_DIRECTORY, 0);
    check("open directory", dirfd >= 0, 1);
    fd = SC5(SYS_openat, dirfd, "f", O_CREAT | O_WRONLY, 0644, 0);
    check("openat", fd >= 0, 1);
    SC(SYS_close, fd, 0, 0);
    check("created in the directory", SC(SYS_stat, "/tmp/d/f", sb, 0), 0);
    check("fstatat", SC5(SYS_fstatat, dirfd, "f", sb, 0, 0), 0);
    check("fstatat regular", ST(sb)->st_mode & S_IFMT, S_IFREG);
    check("fstatat AT_FDCWD", SC5(SYS_fstatat, AT_FDCWD, "/tmp/d/f", sb, 0, 0), 0);
    check("openat bad dirfd", SC5(SYS_openat, 50, "f", O_RDONLY, 0, 0), -EBADF);
    fd = SC(SYS_open, "/tmp/a", O_RDONLY, 0);
    check("openat file as dirfd", SC5(SYS_openat, fd, "f", O_RDONLY, 0, 0), -ENOTDIR);
    SC(SYS_close, fd, 0, 0);
    check("openat absolute ignores dirfd", SC5(SYS_fstatat, 50, "/tmp/a", sb, 0, 0), 0);
    check("mknodat", SC5(SYS_mknodat, dirfd, "sub", S_IFDIR | 0755, 0, 0), 0);
    check("faccessat", SC5(SYS_faccessat, dirfd, "f", 4 | 2 /* R_OK | W_OK */, 0, 0), 0);
    check("root X_OK without x bits", SC5(SYS_faccessat, dirfd, "f", 1 /* X_OK */, 0, 0), -EACCES);

    /* the directory descriptor survives fork and dup */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        int dup = SC(SYS_fcntl, dirfd, F_DUPFD, 30);
        SC(SYS_close, dirfd, 0, 0);
        SC(SYS_exit, SC5(SYS_fstatat, dup, "f", sb, 0, 0) == 0 ? 3 : 4, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    check("dirfd after fork and dup", st >> 8, 3);

    char cwd[64];
    check("fchdir", SC(SYS_fchdir, dirfd, 0, 0), 0);
    SC(SYS_getcwd, cwd, sizeof(cwd), 0);
    check("cwd after fchdir", cwd[0] == '/' && cwd[5] == 'd' && cwd[6] == 0, 1);
    check("relative open in new cwd", SC(SYS_stat, "f", sb, 0), 0);
    SC(SYS_chdir, "/", 0, 0);

    check("unlink a directory", SC(SYS_unlink, "/tmp/d", 0, 0), -EISDIR);
    check("rmdir not empty", SC(SYS_rmdir, "/tmp/d", 0, 0), -ENOTEMPTY);
    check("rmdir a file", SC(SYS_rmdir, "/tmp/d/f", 0, 0), -ENOTDIR);
    check("rmdir mountpoint", SC(SYS_rmdir, "/tmp", 0, 0), -EBUSY);
    check("unlinkat AT_REMOVEDIR", SC(SYS_unlinkat, dirfd, "sub", AT_REMOVEDIR), 0);

    /* hard links */
    check("link", SC(SYS_link, "/tmp/a", "/tmp/d/b", 0), 0);
    SC(SYS_stat, "/tmp/a", sb, 0);
    check("nlink 2", ST(sb)->st_nlink, 2);
    check("link existing", SC(SYS_link, "/tmp/a", "/tmp/d/b", 0), -EEXIST);
    check("link a directory", SC(SYS_link, "/tmp/d", "/tmp/e", 0), -EPERM);
    check("link across filesystems", SC(SYS_link, "/tmp/a", "/a", 0), -EXDEV);
    check("unlink first name", SC(SYS_unlink, "/tmp/a", 0, 0), 0);
    fd = SC(SYS_open, "/tmp/d/b", O_RDONLY, 0);
    char data[8] = {0};
    check("other name still reads", SC(SYS_read, fd, data, 8), 5);
    SC(SYS_close, fd, 0, 0);

    /* symbolic links */
    check("symlinkat", SC(SYS_symlinkat, "b", dirfd, "s"), 0);
    check("symlink absolute", SC(SYS_symlinkat, "/tmp/d", AT_FDCWD, "/tmp/dl"), 0);
    char link[16] = {0};
    check("readlinkat", SC5(SYS_readlinkat, dirfd, "s", link, sizeof(link), 0), 1);
    check("readlinkat contents", link[0], 'b');
    check("readlink not a link", SC5(SYS_readlinkat, dirfd, "b", link, sizeof(link), 0), -EINVAL);
    check("symlink existing", SC(SYS_symlinkat, "x", dirfd, "s"), -EEXIST);
    SC5(SYS_fstatat, dirfd, "s", sb, AT_SYMLINK_NOFOLLOW, 0);
    check("lstat link", ST(sb)->st_mode & S_IFMT, S_IFLNK);
    SC5(SYS_fstatat, dirfd, "s", sb, 0, 0);
    check("stat through link", ST(sb)->st_mode & S_IFMT, S_IFREG);
    check("O_NOFOLLOW", SC5(SYS_openat, dirfd, "s", O_RDONLY | O_NOFOLLOW, 0, 0), -ELOOP);
    check("link in the middle of a path", SC(SYS_stat, "/tmp/dl/b", sb, 0), 0);
    check("link to a link's directory", SC(SYS_stat, "/tmp/dl/s", sb, 0), 0);
    check("create through a link", SC(SYS_mkdir, "/tmp/dl/viadl", 0755, 0), 0);
    check("made in the target", SC(SYS_rmdir, "/tmp/d/viadl", 0, 0), 0);
    /* O_CREAT through a dangling link creates what it points to */
    SC(SYS_symlinkat, "/tmp/d/made", AT_FDCWD, "/tmp/dangling");
    fd = SC(SYS_open, "/tmp/dangling", O_CREAT | O_WRONLY, 0644);
    check("create through a dangling link", fd >= 0, 1);
    SC(SYS_close, fd, 0, 0);
    check("created the target", SC(SYS_stat, "/tmp/d/made", sb, 0), 0);
    SC(SYS_symlinkat, "rel", dirfd, "rl");
    SC(SYS_symlinkat, "/tmp/d/rl", AT_FDCWD, "/tmp/chain");
    fd = SC(SYS_open, "/tmp/chain", O_CREAT | O_WRONLY, 0644);
    check("create through links to a relative link", fd >= 0, 1);
    SC(SYS_close, fd, 0, 0);
    check("relative to the link's directory", SC(SYS_stat, "/tmp/d/rel", sb, 0), 0);
    SC(SYS_symlinkat, "/tmp/d/excl", AT_FDCWD, "/tmp/dangling2");
    check("O_EXCL doesn't create through a link", SC(SYS_open, "/tmp/dangling2", O_CREAT | O_EXCL | O_WRONLY, 0644), -EEXIST);
    check("nothing created", SC(SYS_stat, "/tmp/d/excl", sb, 0), -ENOENT);
    SC(SYS_unlink, "/tmp/d/made", 0, 0);
    SC(SYS_unlink, "/tmp/d/rel", 0, 0);
    SC(SYS_unlink, "/tmp/d/rl", 0, 0);
    SC(SYS_unlink, "/tmp/dangling", 0, 0);
    SC(SYS_unlink, "/tmp/dangling2", 0, 0);
    SC(SYS_unlink, "/tmp/chain", 0, 0);

    /* rename */
    check("rename", SC5(SYS_renameat, dirfd, "b", AT_FDCWD, "/tmp/c", 0), 0);
    check("old name gone", SC(SYS_stat, "/tmp/d/b", sb, 0), -ENOENT);
    check("new name there", SC(SYS_stat, "/tmp/c", sb, 0), 0);
    fd = SC(SYS_open, "/tmp/other", O_CREAT | O_WRONLY, 0644);
    SC(SYS_close, fd, 0, 0);
    check("rename over a file", SC5(SYS_renameat, AT_FDCWD, "/tmp/c", AT_FDCWD, "/tmp/other", 0), 0);
    SC(SYS_stat, "/tmp/other", sb, 0);
    check("replaced", ST(sb)->st_size, 5);
    check("rename a file over a directory", SC5(SYS_renameat, AT_FDCWD, "/tmp/other", AT_FDCWD, "/tmp/d", 0), -EISDIR);
    SC(SYS_mkdir, "/tmp/e", 0755, 0);
    check("rename a directory into itself", SC5(SYS_renameat, AT_FDCWD, "/tmp/d", AT_FDCWD, "/tmp/d/x", 0), -EINVAL);
    check("rename over a non-empty directory", SC5(SYS_renameat, AT_FDCWD, "/tmp/e", AT_FDCWD, "/tmp/d", 0), -ENOTEMPTY);
    check("rename a directory", SC5(SYS_renameat, AT_FDCWD, "/tmp/d", AT_FDCWD, "/tmp/e", 0), 0);
    check("moved with its contents", SC(SYS_stat, "/tmp/e/f", sb, 0), 0);
    check("rename across filesystems", SC5(SYS_renameat, AT_FDCWD, "/tmp/other", AT_FDCWD, "/other", 0), -EXDEV);
    check("rename missing", SC5(SYS_renameat, AT_FDCWD, "/tmp/nothing", AT_FDCWD, "/tmp/x", 0), -ENOENT);

    /* attributes */
    check("chmod", SC(SYS_chmod, "/tmp/other", 04751, 0), 0);
    SC(SYS_stat, "/tmp/other", sb, 0);
    check("mode changed", ST(sb)->st_mode, S_IFREG | 04751);
    check("chmod read-only fs", SC(SYS_chmod, "/init", 0700, 0), -EROFS);
    SC(SYS_symlinkat, "other", AT_FDCWD, "/tmp/ol");
    check("chmod a link's target", SC5(SYS_fchmodat, AT_FDCWD, "/tmp/ol", 04751, 0, 0), 0);
    check("fchmodat AT_SYMLINK_NOFOLLOW", SC5(SYS_fchmodat, AT_FDCWD, "/tmp/ol", 0700, AT_SYMLINK_NOFOLLOW, 0), -EOPNOTSUPP);
    check("chown", SC(SYS_chown, "/tmp/other", 1000, 100), 0);
    SC(SYS_stat, "/tmp/other", sb, 0);
    check("owner changed", ST(sb)->st_uid == 1000 && ST(sb)->st_gid == 100, 1);
    check("lchown", SC(SYS_lchown, "/tmp/ol", 2000, -1), 0);
    SC5(SYS_fstatat, AT_FDCWD, "/tmp/ol", sb, AT_SYMLINK_NOFOLLOW, 0);
    check("link owner changed", ST(sb)->st_uid, 2000);
    SC(SYS_stat, "/tmp/ol", sb, 0);
    check("target owner unchanged", ST(sb)->st_uid, 1000);

    struct aquila_timespec times[2] = {{100, 0}, {200, 0}};
    check("utimensat", SC5(SYS_utimensat, AT_FDCWD, "/tmp/other", times, 0, 0), 0);
    SC(SYS_stat, "/tmp/other", sb, 0);
    check("times set", ST(sb)->st_atim.tv_sec == 100 && ST(sb)->st_mtim.tv_sec == 200, 1);
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_nsec = UTIME_NOW;
    SC5(SYS_utimensat, AT_FDCWD, "/tmp/other", times, 0, 0);
    SC(SYS_stat, "/tmp/other", sb, 0);
    check("UTIME_OMIT and UTIME_NOW", ST(sb)->st_atim.tv_sec == 100 && ST(sb)->st_mtim.tv_sec > 200, 1);
    times[1].tv_nsec = 1000000000;
    check("utimensat bad nsec", SC5(SYS_utimensat, AT_FDCWD, "/tmp/other", times, 0, 0), -EINVAL);
    fd = SC(SYS_open, "/tmp/other", O_RDONLY, 0);
    times[1].tv_nsec = 0;
    check("futimens", SC5(SYS_utimensat, fd, 0, times, 0, 0), 0);
    SC(SYS_close, fd, 0, 0);
    SC(SYS_close, dirfd, 0, 0);

    /* permissions of an unprivileged user */
    SC(SYS_mkdir, "/tmp/rootdir", 0755, 0);
    fd = SC(SYS_open, "/tmp/rootdir/file", O_CREAT | O_WRONLY, 0644);
    SC(SYS_close, fd, 0, 0);
    check("unprivileged file rules (failed step)", aq_child_status(unprivileged_files) >> 8, 0);

    /* setsid: not as a group leader */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        int me = SC(SYS_getpid, 0, 0, 0);
        if (SC(SYS_setsid, 0, 0, 0) != me) SC(SYS_exit, 1, 0, 0);
        if (SC(SYS_getpgrp, 0, 0, 0) != me) SC(SYS_exit, 2, 0, 0);
        if (SC(SYS_setsid, 0, 0, 0) != -EPERM) SC(SYS_exit, 3, 0, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    check("setsid (failed step)", st >> 8, 0);
    check("setsid as a group leader", SC(SYS_setsid, 0, 0, 0), -EPERM);

    /* --- FIFOs --- */
    check("mkfifo", SC(SYS_mknod, "/tmp/fifo", S_IFIFO | 0666, 0), 0);
    SC(SYS_stat, "/tmp/fifo", sb, 0);
    check("stat fifo", ST(sb)->st_mode & S_IFMT, S_IFIFO);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        /* waits for the reader */
        int w = SC(SYS_open, "/tmp/fifo", O_WRONLY, 0);
        SC(SYS_write, w, "fifo", 4);
        SC(SYS_exit, w >= 0 ? 0 : 1, 0, 0);
    }
    aq_spin(10000000);
    fd = SC(SYS_open, "/tmp/fifo", O_RDONLY, 0);
    check("open fifo for reading", fd >= 0, 1);
    char fbuf[8] = {0};
    check("read fifo", SC(SYS_read, fd, fbuf, 8), 4);
    check("fifo data", fbuf[0] == 'f' && fbuf[3] == 'o', 1);
    check("fifo EOF after the writer", SC(SYS_read, fd, fbuf, 8), 0);
    SC(SYS_close, fd, 0, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("fifo writer", st >> 8, 0);
    check("nonblocking writer without readers", SC(SYS_open, "/tmp/fifo", O_WRONLY | O_NONBLOCK, 0), -ENXIO);
    fd = SC(SYS_open, "/tmp/fifo", O_RDONLY | O_NONBLOCK, 0);
    check("nonblocking reader", fd >= 0, 1);
    check("nonblocking reader EOF", SC(SYS_read, fd, fbuf, 8), 0);
    SC(SYS_close, fd, 0, 0);
    /* the reader waits first; the writer opens, writes and closes before
     * it runs again: the reader's open is done (a writer came), it reads
     * what was written (bash's process substitution on a pipeline). The
     * reader's alarm ends it if it waits for ever */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_itimer it = {2000000000ull, 0};
        SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
        int r = SC(SYS_open, "/tmp/fifo", O_RDONLY, 0);
        char rb[8] = {0};
        SC(SYS_exit, r >= 0 && SC(SYS_read, r, rb, 8) == 4 && rb[0] == 'r' ? 0 : 1, 0, 0);
    }
    aq_spin(20000000);
    int w = SC(SYS_open, "/tmp/fifo", O_WRONLY, 0);
    SC(SYS_write, w, "race", 4);
    SC(SYS_close, w, 0, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("fifo: a writer gone before the waiting reader runs", st, 0);

    fd = SC(SYS_open, "/tmp/fifo", O_RDWR, 0);
    check("fifo O_RDWR", fd >= 0, 1);
    check("O_RDWR write", SC(SYS_write, fd, "rw", 2), 2);
    check("O_RDWR read", SC(SYS_read, fd, fbuf, 8), 2);
    SC(SYS_close, fd, 0, 0);

    /* a reader interrupted while waiting leaves no reader behind */
    sa.sa_handler = (uptr) handler;
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        SC(SYS_exit, SC(SYS_open, "/tmp/fifo", O_RDONLY, 0) == -EINTR && got_sig == SIGUSR1 ? 0 : 1, 0, 0);
    aq_spin(10000000);
    SC(SYS_kill, c, SIGUSR1, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("fifo open interrupted", st >> 8, 0);
    check("no reader left", SC(SYS_open, "/tmp/fifo", O_WRONLY | O_NONBLOCK, 0), -ENXIO);
    check("unlink fifo", SC(SYS_unlink, "/tmp/fifo", 0, 0), 0);

    /* --- sync: what the disks have cached (the disk test reads it back) --- */
    check("sync", SC(SYS_sync, 0, 0, 0), 0);

    /* --- reboot (restarting itself is left to the system test) --- */
    check("reboot bad command", SC(SYS_reboot, 0x12345678, 0, 0), -EINVAL);
    check("reboot disable Ctrl+Alt+Del", SC(SYS_reboot, RB_DISABLE_CAD, 0, 0), 0);
    check("reboot enable Ctrl+Alt+Del", SC(SYS_reboot, RB_ENABLE_CAD, 0, 0), 0);

    /* --- flock --- */
    int la = SC(SYS_open, "/tmp/lock", O_CREAT | O_RDWR, 0644);
    int lb = SC(SYS_open, "/tmp/lock", O_RDWR, 0);
    check("flock bad fd", SC(SYS_flock, 99, LOCK_EX, 0), -EBADF);
    check("flock bad op", SC(SYS_flock, la, 0, 0), -EINVAL);
    check("flock exclusive", SC(SYS_flock, la, LOCK_EX, 0), 0);
    check("flock exclusive again", SC(SYS_flock, la, LOCK_EX | LOCK_NB, 0), 0);
    check("flock held by another open", SC(SYS_flock, lb, LOCK_EX | LOCK_NB, 0), -EAGAIN);
    check("flock shared vs exclusive", SC(SYS_flock, lb, LOCK_SH | LOCK_NB, 0), -EAGAIN);
    /* a copy of the descriptor holds the same lock */
    int lc = SC(SYS_fcntl, la, F_DUPFD, 0);
    SC(SYS_close, la, 0, 0);
    check("flock kept by the copy", SC(SYS_flock, lb, LOCK_EX | LOCK_NB, 0), -EAGAIN);
    SC(SYS_close, lc, 0, 0);
    check("flock released on the last close", SC(SYS_flock, lb, LOCK_EX | LOCK_NB, 0), 0);
    /* shared locks */
    check("flock downgrade", SC(SYS_flock, lb, LOCK_SH, 0), 0);
    la = SC(SYS_open, "/tmp/lock", O_RDONLY, 0);
    check("flock two shared", SC(SYS_flock, la, LOCK_SH | LOCK_NB, 0), 0);
    check("flock exclusive vs shared", SC(SYS_flock, la, LOCK_EX | LOCK_NB, 0), -EAGAIN);
    check("flock unlock", SC(SYS_flock, lb, LOCK_UN, 0), 0);
    check("flock upgrade", SC(SYS_flock, la, LOCK_EX | LOCK_NB, 0), 0);
    /* waiting for the lock: the child gets it once it is released */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        int l = SC(SYS_open, "/tmp/lock", O_RDWR, 0);
        int busy = SC(SYS_flock, l, LOCK_EX | LOCK_NB, 0) == -EAGAIN;
        SC(SYS_exit, busy && SC(SYS_flock, l, LOCK_EX, 0) == 0 ? 0 : 1, 0, 0);
    }
    aq_spin(10000000);
    SC(SYS_flock, la, LOCK_UN, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("flock waits for the holder", st >> 8, 0);
    SC(SYS_close, la, 0, 0);
    SC(SYS_close, lb, 0, 0);
    SC(SYS_unlink, "/tmp/lock", 0, 0);

    int fp[2];
    SC(SYS_pipe, fp, 0, 0);
    SC(SYS_fstat, fp[0], sb, 0);
    check("pipe is a FIFO", ST(sb)->st_mode & S_IFMT, S_IFIFO);
    SC(SYS_close, fp[0], 0, 0);
    SC(SYS_close, fp[1], 0, 0);

    /* --- sessions and controlling terminals --- */
    SC(SYS_mknod, "/dev/tty", S_IFCHR | 0666, DEV(5, 0));
    SC(SYS_mknod, "/dev/ptmx", S_IFCHR | 0666, DEV(5, 2));
    SC(SYS_mkdir, "/dev/pts", 0755, 0);
    check("mount devpts", aq_mount("devpts", "/dev/pts", 0), 0);
    int hp[2];
    SC(SYS_pipe, hp, 0, 0);
    hup_pipe = hp[1];
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        pty_session();
    SC(SYS_close, hp[1], 0, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("pty session (failed step)", st >> 8, 0);
    char hup = 0;
    check("SIGHUP to the foreground group", SC(SYS_read, hp[0], &hup, 1) == 1 && hup == 'H', 1);
    SC(SYS_close, hp[0], 0, 0);

    /* every end closed: the pty went away, its id is free again */
    aq_spin(10000000);
    check("/dev/pts/0 removed", SC(SYS_stat, "/dev/pts/0", sb, 0), -ENOENT);
    int ptm = SC(SYS_open, "/dev/ptmx", O_RDWR | O_NOCTTY, 0), ptn = -1;
    SC(SYS_ioctl, ptm, TIOCGPTN, &ptn);
    check("pty id reused", ptn, 0);
    check("TIOCSPGRP not our terminal", SC(SYS_ioctl, ptm, TIOCSPGRP, &c), -ENOTTY);
    check("an ioctl terminals don't have", SC(SYS_ioctl, ptm, 0x5401 + 0x1000, &c), -ENOTTY);

    /* two readers waiting for the terminal's output: they wait, and don't
     * wake each other up again and again, which kept the kernel busy */
    int rd[2];
    for (int i = 0; i < 2; ++i) {
        if (!(rd[i] = SC(SYS_fork, 0, 0, 0))) {
            char b[16];
            SC(SYS_read, ptm, b, sizeof(b));
            SC(SYS_exit, 0, 0, 0);
        }
    }
    struct aquila_timespec rts = {0, 200000000};
    check("pty: two readers waiting on the master, the others run", SC(SYS_nanosleep, &rts, 0, 0), 0);
    for (int i = 0; i < 2; ++i) {
        SC(SYS_kill, rd[i], SIGKILL, 0);
        SC(SYS_waitpid, rd[i], &st, 0);
    }
    SC(SYS_close, ptm, 0, 0);
    check("closed pty removed", SC(SYS_stat, "/dev/pts/0", sb, 0), -ENOENT);
    tty_output_modes();

    /* --- stopping and continuing --- */
    /* the child runs on after being continued, until it is killed (an
     * exit would be reported before the continue) */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        SC(SYS_kill, SC(SYS_getpid, 0, 0, 0), SIGSTOP, 0);
        spin_forever();
    }
    check("waitpid stopped", SC(SYS_waitpid, c, &st, WUNTRACED), c);
    check("stopped by SIGSTOP", st, (SIGSTOP << 8) | 0x7f);
    check("not reported twice", SC(SYS_waitpid, c, &st, WUNTRACED | WNOHANG), 0);
    check("SIGCONT", SC(SYS_kill, c, SIGCONT, 0), 0);
    check("waitpid continued", SC(SYS_waitpid, c, &st, WCONTINUED), c);
    check("continued status", st, 0xffff);
    SC(SYS_kill, c, SIGKILL, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("ran on after continuing", st & 0x7f, SIGKILL);

    /* signals to a stopped process wait until it continues */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        spin_forever();
    SC(SYS_kill, c, SIGSTOP, 0);
    SC(SYS_waitpid, c, &st, WUNTRACED);
    SC(SYS_kill, c, SIGTERM, 0);
    aq_spin(10000000);
    check("stopped process keeps signals pending", SC(SYS_waitpid, c, &st, WNOHANG), 0);
    SC(SYS_kill, c, SIGCONT, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("delivered once continued", st & 0x7f, SIGTERM);

    /* SIGKILL ends a stopped process */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        spin_forever();
    SC(SYS_kill, c, SIGSTOP, 0);
    SC(SYS_waitpid, c, &st, WUNTRACED);
    SC(SYS_kill, c, SIGKILL, 0);
    check("kill a stopped process", SC(SYS_waitpid, c, &st, 0) == c && (st & 0x7f) == SIGKILL, 1);

    /* a caught SIGTSTP doesn't stop */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_sigaction tsa = {(uptr) sigtstp_handler, 0, 0};
        SC(SYS_sigaction, SIGTSTP, &tsa, 0);
        spin_forever();
    }
    aq_spin(10000000);
    SC(SYS_kill, c, SIGTSTP, 0);
    SC(SYS_waitpid, c, &st, WUNTRACED);
    check("SIGTSTP handler", st, 9 << 8);

    /* the terminal's line discipline and job control, on a pty */
    check("line discipline (failed step)", aq_child_status(line_discipline) >> 8, 0);

    /* --- select and poll --- */
    int pp[2];
    SC(SYS_pipe, pp, 0, 0);
    struct fdset rs = {{1UL << pp[0], 0}}, ws = {{1UL << pp[1], 0}};
    check("select pipe writable", do_select(pp[1] + 1, 0, &ws, 0), 1);
    int t1 = now_ms();
    check("select times out", do_select(pp[0] + 1, &rs, 0, 200000), 0);
    int waited = now_ms() - t1;
    check("waited 200ms", waited >= 190 && waited < 1000, 1);
    check("nothing ready", rs.bits[0], 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_timespec d = {0, 100000000};
        SC(SYS_nanosleep, &d, 0, 0);
        SC(SYS_write, pp[1], "w", 1);
        SC(SYS_exit, 0, 0, 0);
    }
    rs.bits[0] = 1UL << pp[0];
    t1 = now_ms();
    check("select wakes up for data", do_select(pp[0] + 1, &rs, 0, -1), 1);
    check("woke before a second", now_ms() - t1 < 1000, 1);
    check("read end set", rs.bits[0], 1UL << pp[0]);
    SC(SYS_waitpid, c, &st, 0);
    struct fdset bad = {{0, 0}};  /* descriptor 40 */
    bad.bits[40 / (8 * sizeof(long))] = 1UL << (40 % (8 * sizeof(long)));
    check("select closed fd", do_select(41, &bad, 0, 0), -EBADF);
    fd = SC(SYS_open, "/tmp/other", O_RDONLY, 0);
    rs.bits[0] = 1UL << fd;
    check("regular file always ready", do_select(fd + 1, &rs, 0, -1), 1);
    SC(SYS_close, fd, 0, 0);

    struct pollfd pfd[3] = {{pp[0], POLLIN, 0}, {pp[1], POLLOUT, 0}, {45, POLLIN, 0}};
    check("poll", SC(SYS_poll, pfd, 3, 0), 3);
    check("poll POLLIN", pfd[0].revents, POLLIN);
    check("poll POLLOUT", pfd[1].revents, POLLOUT);
    check("poll POLLNVAL", pfd[2].revents, POLLNVAL);
    char pc;
    SC(SYS_read, pp[0], &pc, 1);
    pfd[1].fd = -1;
    t1 = now_ms();
    check("poll times out", SC(SYS_poll, pfd, 2, 150), 0);
    check("waited 150ms", now_ms() - t1 >= 140, 1);
    SC(SYS_close, pp[1], 0, 0);
    check("poll end of pipe", SC(SYS_poll, pfd, 1, -1) == 1 && (pfd[0].revents & POLLIN), 1);
    SC(SYS_close, pp[0], 0, 0);

    check("terminal read timeout (failed step)", aq_child_status(read_timeout) >> 8, 0);

    /* --- clocks and nanosleep --- */
    struct aquila_timespec ts = {0, 0}, left = {0, 0};
    check("clock_gettime realtime", SC(SYS_clock_gettime, CLOCK_REALTIME, &ts, 0), 0);

    /* finer than the timer's tick (2 ms): the HPET or the TSC count it */
    {
        struct aquila_timespec a, b;
        int finer = 0;

        for (int i = 0; i < 100000 && !finer; ++i) {
            SC(SYS_clock_gettime, CLOCK_MONOTONIC, &a, 0);
            SC(SYS_clock_gettime, CLOCK_MONOTONIC, &b, 0);
            long d = (b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec);
            finer = d > 0 && d < 1000000;
        }

        check("clock_gettime monotonic: finer than a tick", finer, 1);
    }
    check("realtime is recent", ts.tv_sec > 1500000000, 1);
    check("clock_gettime bad clock", SC(SYS_clock_gettime, 99, &ts, 0), -EINVAL);
    check("clock_gettime EFAULT", SC(SYS_clock_gettime, CLOCK_MONOTONIC, KADDR, 0), -EFAULT);

    int t0 = now_ms();
    ts = (struct aquila_timespec) {0, 200000000};
    check("nanosleep", SC(SYS_nanosleep, &ts, 0, 0), 0);
    int slept = now_ms() - t0;
    check("slept 200ms", slept >= 200 && slept < 1000, 1);
    /* the real time clock counts below the second */
    struct aquila_timespec r0, r1;
    SC(SYS_clock_gettime, CLOCK_REALTIME, &r0, 0);
    ts = (struct aquila_timespec) {0, 100000000};
    SC(SYS_nanosleep, &ts, 0, 0);
    SC(SYS_clock_gettime, CLOCK_REALTIME, &r1, 0);
    long long rns = (long long) (r1.tv_sec - r0.tv_sec) * 1000000000 + r1.tv_nsec - r0.tv_nsec;
    check("realtime ns", rns >= 90000000 && rns < 900000000, 1);

    /* the interval timer: SIGALRM every 50ms, read, then disarmed */
    struct aquila_sigaction asa = {(uptr) alarm_handler, 0, 0};
    SC(SYS_sigaction, SIGALRM, &asa, 0);
    struct aquila_itimer it = {50000000, 50000000}, iold = {1, 1};
    check("setitimer kaddr", SC(SYS_setitimer, AQUILA_ITIMER_REAL, KADDR, 0), -EFAULT);
    check("setitimer virtual", SC(SYS_setitimer, 1, &it, 0), -EINVAL);
    check("setitimer", SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, &iold), 0);
    check("setitimer none before", (int) (iold.value | iold.interval), 0);
    int a0 = now_ms();
    while (now_ms() - a0 < 330) {
        ts = (struct aquila_timespec) {0, 10000000};
        SC(SYS_nanosleep, &ts, 0, 0);
    }
    check("SIGALRM every 50ms", alarms >= 4 && alarms <= 8, 1);
    check("getitimer", SC(SYS_setitimer, AQUILA_ITIMER_REAL, 0, &iold), 0);
    check("getitimer left", iold.value > 0 && iold.value <= 50000000, 1);
    check("getitimer interval", (int) (iold.interval == 50000000), 1);
    it = (struct aquila_itimer) {0, 0};
    SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
    int a1 = alarms;
    ts = (struct aquila_timespec) {0, 150000000};
    SC(SYS_nanosleep, &ts, 0, 0);
    check("disarmed", alarms, a1);

    /* once, in a child: not inherited by fork() */
    it = (struct aquila_itimer) {30000000, 0};
    SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
    int apid = SC(SYS_fork, 0, 0, 0);
    if (!apid) {
        SC(SYS_setitimer, AQUILA_ITIMER_REAL, 0, &iold);
        SC(SYS_exit, iold.value != 0, 0, 0);
    }
    int ast2 = 0;
    SC(SYS_waitpid, apid, &ast2, 0);
    check("timer not inherited", ast2 >> 8, 0);
    ts = (struct aquila_timespec) {0, 100000000};
    SC(SYS_nanosleep, &ts, 0, 0);
    check("one shot", alarms, a1 + 1);

    /* SA_RESTART: a read the signal comes in is made again, without it
     * it fails with EINTR */
    int rp[2];
    char rb = 0;
    SC(SYS_pipe, rp, 0, 0);
    for (int restart = 1; restart >= 0; --restart) {
        asa = (struct aquila_sigaction) {(uptr) alarm_handler, 0, restart ? SA_RESTART : 0};
        SC(SYS_sigaction, SIGALRM, &asa, 0);
        c = SC(SYS_fork, 0, 0, 0);
        if (!c) {
            struct aquila_timespec d = {0, 150000000};
            SC(SYS_nanosleep, &d, 0, 0);
            SC(SYS_write, rp[1], "r", 1);
            SC(SYS_exit, 0, 0, 0);
        }
        a1 = alarms;
        it = (struct aquila_itimer) {50000000, 0};
        SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
        int got = SC(SYS_read, rp[0], &rb, 1);
        check(restart ? "SA_RESTART read restarted" : "read interrupted", got, restart ? 1 : -EINTR);
        check("the handler ran", alarms, a1 + 1);
        SC(SYS_waitpid, c, &st, 0);
        if (!restart)
            SC(SYS_read, rp[0], &rb, 1);
    }
    SC(SYS_close, rp[0], 0, 0);
    SC(SYS_close, rp[1], 0, 0);

    /* a wait with a deadline that a handler interrupts doesn't leave
     * the thread with the timer (it exits, the timer must not find it) */
    SC(SYS_pipe, rp, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct pollfd tpf = {rp[0], POLLIN, 0};
        it = (struct aquila_itimer) {30000000, 0};
        SC(SYS_setitimer, AQUILA_ITIMER_REAL, &it, 0);
        SC(SYS_exit, SC(SYS_poll, &tpf, 1, 400) == -EINTR, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    check("timed poll interrupted", st >> 8, 1);
    ts = (struct aquila_timespec) {0, 500000000};
    check("its deadline passes", SC(SYS_nanosleep, &ts, 0, 0), 0);
    SC(SYS_close, rp[0], 0, 0);
    SC(SYS_close, rp[1], 0, 0);

    /* children of a parent that ignores SIGCHLD, or has SA_NOCLDWAIT,
     * don't stay zombies: there is nothing to wait for */
    struct aquila_sigaction csa = {1 /* SIG_IGN */, 0, 0};
    SC(SYS_sigaction, SIGCHLD, &csa, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        SC(SYS_exit, 0, 0, 0);
    ts = (struct aquila_timespec) {0, 50000000};
    SC(SYS_nanosleep, &ts, 0, 0);
    check("SIGCHLD ignored, no zombie", SC(SYS_waitpid, c, &st, 0), -ECHILD);
    got_sig = 0;
    csa = (struct aquila_sigaction) {(uptr) handler, 0, SA_NOCLDWAIT};
    SC(SYS_sigaction, SIGCHLD, &csa, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        SC(SYS_exit, 0, 0, 0);
    SC(SYS_nanosleep, &ts, 0, 0);
    check("SA_NOCLDWAIT, no zombie", SC(SYS_waitpid, c, &st, 0), -ECHILD);
    check("SA_NOCLDWAIT, SIGCHLD still sent", got_sig, SIGCHLD);
    csa = (struct aquila_sigaction) {0 /* SIG_DFL */, 0, 0};
    SC(SYS_sigaction, SIGCHLD, &csa, 0);

    /* getrandom */
    unsigned char r0b[16] = {0}, r1b[16] = {0};
    check("getrandom kaddr", SC(SYS_getrandom, KADDR, 16, 0), -EFAULT);
    check("getrandom", SC(SYS_getrandom, r0b, 16, 0), 16);
    SC(SYS_getrandom, r1b, 16, 0);
    int rsame = 0, rzero = 0;
    for (int i = 0; i < 16; ++i) {
        rsame += r0b[i] == r1b[i];
        rzero += !r0b[i];
    }
    check("getrandom differs", rsame < 8 && rzero < 8, 1);

    ts.tv_nsec = 1000000000;
    check("nanosleep bad nsec", SC(SYS_nanosleep, &ts, 0, 0), -EINVAL);

    /* two sleepers wake up at their own times */
    t0 = now_ms();
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_timespec d = {0, 100000000};
        SC(SYS_nanosleep, &d, 0, 0);
        SC(SYS_exit, now_ms() - t0 < 300 ? 0 : 1, 0, 0);
    }
    ts = (struct aquila_timespec) {0, 400000000};
    SC(SYS_nanosleep, &ts, 0, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("child woke first", st >> 8, 0);

    /* CPU time: our own, then a child's once waited for */
    struct aquila_tms tms;
    aq_spin(50000000);
    check("times", SC(SYS_times, &tms, 0, 0) > 0, 1);
    check("user time counted", tms.tms_utime > 0, 1);
    u32 cutime = tms.tms_cutime;
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        aq_spin(50000000);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_times, &tms, 0, 0);
    check("child's time added", tms.tms_cutime > cutime, 1);
    check("times EFAULT", SC(SYS_times, KADDR, 0, 0), -EFAULT);

    /* interrupted by a signal: EINTR and the time left */
    sa.sa_handler = (uptr) handler;
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_timespec d = {0, 100000000};
        SC(SYS_nanosleep, &d, 0, 0);
        SC(SYS_kill, SC(SYS_getppid, 0, 0, 0), SIGUSR1, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    got_sig = 0;
    ts = (struct aquila_timespec) {10, 0};
    t0 = now_ms();
    check("nanosleep interrupted", SC(SYS_nanosleep, &ts, &left, 0), -EINTR);
    check("handler ran", got_sig, SIGUSR1);
    check("woke early", now_ms() - t0 < 2000, 1);
    check("time left", left.tv_sec >= 8 && left.tv_sec <= 9, 1);
    SC(SYS_waitpid, c, &st, 0);

    /* --- signal masks --- */
    unsigned long set = 1UL << SIGUSR1, old = 0;
    sa.sa_handler = (uptr) handler;
    SC(SYS_sigaction, SIGUSR1, &sa, 0);
    got_sig = 0;
    check("block SIGUSR1", SC(SYS_sigmask, SIG_BLOCK, &set, &old), 0);
    check("old mask empty", old, 0);
    SC(SYS_kill, pid, SIGUSR1, 0);
    check("blocked signal pending", got_sig, 0);
    check("unblock SIGUSR1", SC(SYS_sigmask, SIG_UNBLOCK, &set, 0), 0);
    check("delivered on unblock", got_sig, SIGUSR1);
    unsigned long kset = 1UL << SIGKILL;
    SC(SYS_sigmask, SIG_BLOCK, &kset, 0);
    SC(SYS_sigmask, SIG_BLOCK, 0, &old);
    check("SIGKILL can't be blocked", old & kset, 0);
    check("sigmask bad how", SC(SYS_sigmask, 7, &set, 0), -EINVAL);

    /* sigsuspend: woken by a signal from a child */
    got_sig = 0;
    SC(SYS_sigmask, SIG_BLOCK, &set, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        aq_spin(20000000);
        SC(SYS_kill, SC(SYS_getppid, 0, 0, 0), SIGUSR1, 0);
        SC(SYS_exit, 0, 0, 0);
    }
    unsigned long none = 0;
    check("sigsuspend -> EINTR", SC(SYS_sigsuspend, &none, 0, 0), -EINTR);
    check("sigsuspend handler ran", got_sig, SIGUSR1);
    SC(SYS_sigmask, SIG_BLOCK, 0, &old);
    check("sigsuspend restores mask", old, set);
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_sigmask, SIG_SETMASK, &none, 0);

    /* a blocked read interrupted by a signal handler fails with EINTR */
    int ip[2];
    SC(SYS_pipe, ip, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        aq_spin(20000000);
        SC(SYS_kill, SC(SYS_getppid, 0, 0, 0), SIGUSR1, 0);
        aq_spin(20000000);
        SC(SYS_exit, 0, 0, 0);
    }
    check("blocked read -> EINTR", SC(SYS_read, ip[0], lbuf, 1), -EINTR);
    SC(SYS_waitpid, c, &st, 0);
    SC(SYS_close, ip[0], 0, 0);
    SC(SYS_close, ip[1], 0, 0);

    /* --- processes --- */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c)
        SC(SYS_exit, SC(SYS_getppid, 0, 0, 0) == pid ? 0 : 1, 0, 0);
    SC(SYS_waitpid, c, &st, 0);
    check("getppid", st >> 8, 0);

    /* process groups: a child in its own group, signalled through it */
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        struct aquila_sigaction s3 = {(uptr) child_handler, 0, 0};
        SC(SYS_sigaction, SIGUSR1, &s3, 0);
        for (;;);
    }
    check("setpgid child", SC(SYS_setpgid, c, c, 0), 0);
    check("setpgid unknown pid", SC(SYS_setpgid, 3999, 3999, 0), -ESRCH);
    /* this test runs as init, the session leader */
    check("setpgid session leader", SC(SYS_setpgid, 0, 0, 0), -EPERM);
    check("join missing group", SC(SYS_setpgid, 0, 4000, 0), -EPERM);
    aq_spin(10000000);
    check("kill group", SC(SYS_kill, -c, SIGUSR1, 0), 0);
    check("group child wait", SC(SYS_waitpid, c, &st, 0), c);
    check("group child status", st, 7 << 8);
    check("kill missing group", SC(SYS_kill, -3999, SIGUSR1, 0), -ESRCH);

    /* --- copy-on-write isolation (CR0.WP) --- */
    static char cow[4096] __attribute__((aligned(4096)));
    /* fault the page in, fork then shares it read-only */
    for (int i = 0; i < 6; ++i)
        ((volatile char *) cow)[i] = "parent"[i];
    int cp[2], go[2];
    char gobyte;
    SC(SYS_pipe, cp, 0, 0);
    SC(SYS_pipe, go, 0, 0);
    c = SC(SYS_fork, 0, 0, 0);
    if (!c) {
        /* look at the page only after the parent wrote to its own copy */
        SC(SYS_read, go[0], &gobyte, 1);
        SC(SYS_exit, ((volatile char *) cow)[0], 0, 0);
    }
    /* fork left the page read-only in the parent, the kernel write must
     * fault and copy it instead of going into the page the child shares */
    SC(SYS_write, cp[1], "PARENT", 6);
    SC(SYS_read, cp[0], cow, 6);
    SC(SYS_write, go[1], "g", 1);
    SC(SYS_waitpid, c, &st, 0);
    check("kernel write didn't leak into child", (st >> 8) & 0xff, 'p');

    /* --- virtual terminals --- */
    SC(SYS_mknod, "/dev/tty0", S_IFCHR | 0600, DEV(4, 0));
    for (int i = 1; i <= 6; ++i)
        SC(SYS_mknod, vt_path(i), S_IFCHR | 0600, DEV(4, i));

    int vt0 = SC(SYS_open, "/dev/tty0", O_RDWR | O_NOCTTY, 0);
    check("open /dev/tty0", vt0 >= 0, 1);

    char kbtype = 0;
    int vi = -1;
    check("KDGKBTYPE", SC(SYS_ioctl, vt0, KDGKBTYPE, &kbtype), 0);
    check("KDGKBTYPE KB_101", kbtype, KB_101);
    check("KDGETMODE kaddr", SC(SYS_ioctl, vt0, KDGETMODE, KADDR), -EFAULT);
    check("KDGETMODE", SC(SYS_ioctl, vt0, KDGETMODE, &vi), 0);
    check("KDGETMODE text", vi, KD_TEXT);
    check("KDSETMODE bad", SC(SYS_ioctl, vt0, KDSETMODE, 7), -EINVAL);
    check("KDGKBMODE", SC(SYS_ioctl, vt0, KDGKBMODE, &vi), 0);
    check("KDGKBMODE xlate", vi, K_XLATE);
    check("KDSKBMODE bad", SC(SYS_ioctl, vt0, KDSKBMODE, 9), -EINVAL);
    check("VT_GETSTATE kaddr", SC(SYS_ioctl, vt0, VT_GETSTATE, KADDR), -EFAULT);
    check("VT_GETSTATE active", vt_active(vt0), 1);
    check("VT_ACTIVATE 0", SC(SYS_ioctl, vt0, VT_ACTIVATE, 0), -ENXIO);
    check("VT_DISALLOCATE", SC(SYS_ioctl, vt0, VT_DISALLOCATE, 4), 0);

    /* the keymap, as Linux's key tables */
    struct kbentry kbe = {0, 30, 0};
    check("KDGKBENT kaddr", SC(SYS_ioctl, vt0, KDGKBENT, KADDR), -EFAULT);
    check("KDGKBENT", SC(SYS_ioctl, vt0, KDGKBENT, &kbe), 0);
    check("KDGKBENT a", kbe.kb_value, K(KT_LETTER, 'a'));
    kbe = (struct kbentry) {1 << KG_SHIFT, 2, 0};
    SC(SYS_ioctl, vt0, KDGKBENT, &kbe);
    check("KDGKBENT shift 1", kbe.kb_value, K(KT_LATIN, '!'));
    kbe = (struct kbentry) {0, 59, 0};
    SC(SYS_ioctl, vt0, KDGKBENT, &kbe);
    check("KDGKBENT F1", kbe.kb_value, K_F1);
    kbe = (struct kbentry) {0, 103, 0};
    SC(SYS_ioctl, vt0, KDGKBENT, &kbe);
    check("KDGKBENT Up", kbe.kb_value, K_UP);
    kbe = (struct kbentry) {0, 42, 0};
    SC(SYS_ioctl, vt0, KDGKBENT, &kbe);
    check("KDGKBENT Shift", kbe.kb_value, K_SHIFT);
    kbe = (struct kbentry) {1 << KG_CTRL, 30, 0};
    SC(SYS_ioctl, vt0, KDGKBENT, &kbe);
    check("KDGKBENT no Ctrl table", kbe.kb_value, K_HOLE);
    check("VT_ACTIVATE 7", SC(SYS_ioctl, vt0, VT_ACTIVATE, 7), -ENXIO);

    /* a freevt terminal, in use once it is open */
    int freevt = -1, again = -1;
    check("VT_OPENQRY", SC(SYS_ioctl, vt0, VT_OPENQRY, &freevt), 0);
    check("VT_OPENQRY found one", freevt > 1 && freevt <= 6, 1);
    int tn = SC(SYS_open, vt_path(freevt), O_RDWR | O_NOCTTY, 0);
    SC(SYS_ioctl, vt0, VT_OPENQRY, &again);
    check("VT_OPENQRY skips open ones", again != freevt, 1);
    struct vt_stat vs = {0};
    SC(SYS_ioctl, vt0, VT_GETSTATE, &vs);
    check("VT_GETSTATE open", (vs.v_state >> freevt) & 1, 1);

    /* the modes of a process that is gone are undone */
    int vc = SC(SYS_fork, 0, 0, 0);
    if (!vc) {
        int f = SC(SYS_open, "/dev/tty1", O_RDWR | O_NOCTTY, 0);
        SC(SYS_ioctl, f, KDSETMODE, KD_GRAPHICS);
        SC(SYS_ioctl, f, KDSKBMODE, K_MEDIUMRAW);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, vc, &ast, 0);
    int vt1 = SC(SYS_open, "/dev/tty1", O_RDWR | O_NOCTTY, 0);
    SC(SYS_ioctl, vt1, KDGETMODE, &vi);
    check("KD_GRAPHICS left by a child", vi, KD_GRAPHICS);
    check("VT_ACTIVATE", SC(SYS_ioctl, vt0, VT_ACTIVATE, freevt), 0);
    check("VT_WAITACTIVE", SC(SYS_ioctl, vt0, VT_WAITACTIVE, freevt), 0);
    check("switched", vt_active(vt0), freevt);
    SC(SYS_ioctl, vt1, KDGETMODE, &vi);
    check("KD_TEXT once the child is gone", vi, KD_TEXT);
    SC(SYS_ioctl, vt1, KDGKBMODE, &vi);
    check("K_XLATE once the child is gone", vi, K_XLATE);

    /* the foreground terminal's own modes */
    check("KDSETMODE graphics", SC(SYS_ioctl, tn, KDSETMODE, KD_GRAPHICS), 0);
    SC(SYS_ioctl, vt0, KDGETMODE, &vi);
    check("/dev/tty0 is the foreground", vi, KD_GRAPHICS);
    check("KDSKBMODE raw", SC(SYS_ioctl, tn, KDSKBMODE, K_RAW), 0);
    SC(SYS_ioctl, tn, KDGKBMODE, &vi);
    check("KDGKBMODE raw", vi, K_RAW);

    /* VT_PROCESS: switching away waits for VT_RELDISP */
    struct aquila_sigaction vsa = {(uptr) vt_handler, 0, 0};
    SC(SYS_sigaction, SIGUSR1, &vsa, 0);
    SC(SYS_sigaction, SIGUSR2, &vsa, 0);
    struct vt_mode vm = {VT_PROCESS, 0, SIGUSR1, SIGUSR2, 0}, vm2 = {0};
    check("VT_SETMODE bad", SC(SYS_ioctl, tn, VT_SETMODE, &(struct vt_mode) {5}), -EINVAL);
    check("VT_SETMODE", SC(SYS_ioctl, tn, VT_SETMODE, &vm), 0);
    SC(SYS_ioctl, tn, VT_GETMODE, &vm2);
    check("VT_GETMODE", vm2.mode == VT_PROCESS && vm2.relsig == SIGUSR1 && vm2.acqsig == SIGUSR2, 1);
    check("VT_RELDISP nothing pending", SC(SYS_ioctl, tn, VT_RELDISP, 1), -EINVAL);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, 1);
    check("relsig", vt_rel, 1);
    check("not switched yet", vt_active(vt0), freevt);
    check("VT_RELDISP refuse", SC(SYS_ioctl, tn, VT_RELDISP, 0), 0);
    check("refused", vt_active(vt0), freevt);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, 1);
    check("relsig again", vt_rel, 2);
    check("VT_RELDISP release", SC(SYS_ioctl, tn, VT_RELDISP, 1), 0);
    check("released", vt_active(vt0), 1);
    check("VT_RELDISP in the background", SC(SYS_ioctl, tn, VT_RELDISP, 1), -EINVAL);
    check("no acqsig yet", vt_acq, 0);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, freevt);
    check("acqsig", vt_acq, 1);
    check("VT_RELDISP VT_ACKACQ", SC(SYS_ioctl, tn, VT_RELDISP, VT_ACKACQ), 0);
    vm.mode = VT_AUTO;
    SC(SYS_ioctl, tn, VT_SETMODE, &vm);
    SC(SYS_ioctl, tn, KDSKBMODE, K_XLATE);
    SC(SYS_ioctl, tn, KDSETMODE, KD_TEXT);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, 1);
    check("VT_AUTO", vt_active(vt0), 1);

    /* a VT_PROCESS process that is gone doesn't hold the terminal */
    vc = SC(SYS_fork, 0, 0, 0);
    if (!vc) {
        struct vt_mode cm = {VT_PROCESS, 0, SIGUSR1, SIGUSR2, 0};
        SC(SYS_ioctl, vt1, VT_SETMODE, &cm);
        SC(SYS_exit, 0, 0, 0);
    }
    SC(SYS_waitpid, vc, &ast, 0);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, freevt);
    check("VT_PROCESS of a child that is gone", vt_active(vt0), freevt);
    SC(SYS_ioctl, vt0, VT_ACTIVATE, 1);

    SC(SYS_close, tn, 0, 0);
    SC(SYS_ioctl, vt0, VT_OPENQRY, &again);
    check("VT_OPENQRY once closed", again, freevt);
    SC(SYS_close, vt1, 0, 0);
    SC(SYS_close, vt0, 0, 0);

    exit_path_signals();
    user_memory();
    orphans();
    open_files();
    fpu_threads();
    inode_numbers();
    check("exec: the other threads end, the new image runs alone",
          aq_child_status(exec_with_threads), 0x55 << 8);
    stopped_sleeps();
    vnode_paths();

    wait_and_mount();

    aq_end();
}
