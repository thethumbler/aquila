/*
 * File reads and writes, timed: the page cache and tmpfs
 *
 * An ext2 disk with 4 KiB blocks is the first ATA disk, mounted on /mnt;
 * tmpfs is on /tmp. On each: a file written 4 KiB at a time, read back
 * the same way, read again, read 64 KiB and 512 bytes at a time, read
 * through a mapping; a small file read again; 200 files made, stat'ed
 * and opened. Each number is printed ("bench <name> <value> <unit>"),
 * tests/run.sh shows some.
 *
 * Timings depend on the machine, so the checks compare numbers of the
 * same run: a file read again comes from memory (the page cache), at
 * least 10 times as fast as from the disk; writing a file to tmpfs a
 * piece at a time isn't much slower than reading it (its buffer grows
 * by half at a time: it used to be copied at every write).
 *
 * Booted with fscache.pressure, in less memory than the file it maps: a
 * 96 MiB file mapped and read through, by two processes at once, and
 * shared; the page cache takes back mapped pages, out of each process's
 * page tables, and a page a process wrote to (its own copy) stays.
 */

#include "../lib/aqtest.h"

#include <aquila/mman.h>

static char buf[1 << 16] __attribute__((aligned(4096)));

static unsigned long now_us(void)
{
    struct aquila_timespec ts;
    SC(SYS_clock_gettime, CLOCK_MONOTONIC, &ts, 0);
    return ts.tv_sec * 1000000ul + ts.tv_nsec / 1000;
}

static void report(const char *fs, const char *name, unsigned long value, const char *unit)
{
    aq_puts("bench ");
    aq_puts(fs);
    aq_puts(".");
    aq_puts(name);
    aq_puts(" ");
    aq_putn(value);
    aq_puts(" ");
    aq_puts(unit);
    aq_puts("\n");
}

/* KiB/s of `bytes` in `us` microseconds */
static unsigned long rate(unsigned long bytes, unsigned long us)
{
    return us ? (bytes / 1024) * 1000000ul / us : 0;
}

/* writes `size` bytes in `chunk`-sized writes, then syncs; KiB/s */
static unsigned long write_file(const char *path, unsigned long size, unsigned long chunk)
{
    int fd = SC(SYS_open, path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    unsigned long t = now_us();

    for (unsigned long done = 0; done < size; done += chunk)
        SC(SYS_write, fd, buf, chunk);

    SC(SYS_close, fd, 0, 0);
    SC(SYS_sync, 0, 0, 0);
    return rate(size, now_us() - t);
}

/* reads a whole file in `chunk`-sized reads; KiB/s */
static unsigned long read_file(const char *path, unsigned long chunk)
{
    int fd = SC(SYS_open, path, O_RDONLY, 0);
    unsigned long total = 0, t = now_us();
    long r;

    while ((r = SC(SYS_read, fd, buf, chunk)) > 0)
        total += r;

    SC(SYS_close, fd, 0, 0);
    return rate(total, now_us() - t);
}

/* reads a file through a mapping, a byte of each page; KiB/s */
static unsigned long mmap_file(const char *path, unsigned long size)
{
    int fd = SC(SYS_open, path, O_RDONLY, 0);
    void *p = 0;
    struct aquila_mmap_args a = {0, size, PROT_READ, MAP_PRIVATE, fd, 0};
    unsigned long t = now_us();

    if (SC(SYS_mmap, &a, &p, 0))
        return 0;

    for (unsigned long off = 0; off < size; off += 4096)
        (void) ((volatile char *) p)[off];

    t = now_us() - t;
    SC(SYS_munmap, p, size, 0);
    SC(SYS_close, fd, 0, 0);
    return rate(size, t);
}

/* "<dir>/<name>" and "<dir>/fNNN" */
static char path[64];

static const char *at(const char *dir, const char *name)
{
    int n = 0;

    while (*dir)
        path[n++] = *dir++;
    path[n++] = '/';
    while (*name)
        path[n++] = *name++;
    path[n] = 0;
    return path;
}

static const char *nth(const char *dir, int i)
{
    char name[] = "f000";

    name[1] = '0' + i / 100;
    name[2] = '0' + i / 10 % 10;
    name[3] = '0' + i % 10;
    return at(dir, name);
}

/* the numbers of a filesystem */
struct numbers {
    unsigned long write, read, again, tmp;
};

static struct numbers suite(const char *fs, const char *dir, unsigned long size)
{
    struct numbers n;
    char st[256];

    report(fs, "write-4k", n.write = write_file(at(dir, "big"), size, 4096), "KiB/s");
    report(fs, "read-4k", n.read = read_file(at(dir, "big"), 4096), "KiB/s");
    report(fs, "read-4k-again", n.again = read_file(at(dir, "big"), 4096), "KiB/s");
    report(fs, "read-64k", read_file(at(dir, "big"), 65536), "KiB/s");
    report(fs, "read-512", read_file(at(dir, "big"), 512), "KiB/s");
    report(fs, "mmap", mmap_file(at(dir, "big"), size), "KiB/s");
    report(fs, "mmap-again", mmap_file(at(dir, "big"), size), "KiB/s");

    /* a small file, which any cache holds */
    write_file(at(dir, "small"), 2ul << 20, 4096);
    read_file(at(dir, "small"), 4096);
    report(fs, "small-read-again", read_file(at(dir, "small"), 4096), "KiB/s");

    /* 200 files made, stat'ed 5 times, opened and read 5 times */
    unsigned long t = now_us();
    for (int i = 0; i < 200; ++i) {
        int fd = SC(SYS_open, nth(dir, i), O_CREAT | O_WRONLY, 0644);
        SC(SYS_write, fd, buf, 100);
        SC(SYS_close, fd, 0, 0);
    }
    report(fs, "create-200", now_us() - t, "us");

    t = now_us();
    for (int k = 0; k < 5; ++k)
        for (int i = 0; i < 200; ++i)
            SC(SYS_stat, nth(dir, i), st, 0);
    report(fs, "stat-1000", now_us() - t, "us");

    t = now_us();
    for (int k = 0; k < 5; ++k)
        for (int i = 0; i < 200; ++i) {
            int fd = SC(SYS_open, nth(dir, i), O_RDONLY, 0);
            SC(SYS_read, fd, buf, 100);
            SC(SYS_close, fd, 0, 0);
        }
    report(fs, "open-read-1000", now_us() - t, "us");

    return n;
}

/* the pressure test's file: each 4 KiB page's words are its number */
#define PRESSURE_SIZE   (96ul << 20)

static void page_words(unsigned long page)
{
    for (unsigned i = 0; i < 4096 / sizeof(u32); ++i)
        ((u32 *) buf)[i] = page;
}

/* whether a mapping of the pressure file reads right, every page */
static int mapped_right(volatile u32 *m)
{
    for (unsigned long page = 0; page < PRESSURE_SIZE / 4096; ++page) {
        volatile u32 *w = m + page * (4096 / sizeof(u32));

        if (w[0] != page || w[511] != page || w[1023] != page)
            return 0;
    }

    return 1;
}

static void *map(int fd, unsigned long size, int prot, int flags, unsigned long off)
{
    void *p = 0;
    struct aquila_mmap_args a = {0, size, prot, flags, fd, off};
    return SC(SYS_mmap, &a, &p, 0) ? 0 : p;
}

static void pressure(void)
{
    int fd = SC(SYS_open, "/mnt/pressure", O_CREAT | O_RDWR | O_TRUNC, 0644);

    for (unsigned long page = 0; page < PRESSURE_SIZE / 4096; ++page) {
        page_words(page);
        SC(SYS_write, fd, buf, 4096);
    }

    SC(SYS_sync, 0, 0, 0);

    /* a page of the file the process writes to: its own copy */
    volatile u32 *own = map(fd, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, 4096 * 7);
    check("pressure: a writable private mapping", own != 0, 1);
    own[0] = 0xdead;

    /* read through, by two processes at once (the child's mapping is a
     * copy of the parent's: the same file, its pages in both) */
    volatile u32 *m = map(fd, PRESSURE_SIZE, PROT_READ, MAP_PRIVATE, 0);
    check("pressure: a mapping bigger than the memory", m != 0, 1);

    int pid = SC(SYS_fork, 0, 0, 0);
    if (!pid)
        SC(SYS_exit, mapped_right(m) && mapped_right(m), 0, 0);

    check("pressure: read through the mapping", mapped_right(m), 1);
    int st = 0;
    SC(SYS_waitpid, pid, &st, 0);
    check("pressure: read through by another process at once", st >> 8, 1);

    /* a shared mapping, read only, reads the same */
    volatile u32 *sh = map(fd, PRESSURE_SIZE, PROT_READ, MAP_SHARED, 0);
    check("pressure: a shared mapping, read only", sh != 0 && mapped_right(sh), 1);
    check("pressure: a shared mapping can't be written yet",
          map(fd, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, 0) == 0, 1);

    /* the copy stays the process's; the file has what was written */
    u32 word = 0;
    SC5(SYS_pread, fd, &word, sizeof(word), 4096 * 7, 0);
    check("pressure: the private copy kept, the file not changed", own[0] == 0xdead && word == 7, 1);

    char text[256] = {0};
    int mi = SC(SYS_open, "/proc/meminfo", O_RDONLY, 0);
    SC(SYS_read, mi, text, sizeof(text) - 1);
    SC(SYS_close, mi, 0, 0);
    aq_puts(text);
}

/* whether the kernel's command line has `word` */
static int cmdline_has(const char *word)
{
    static char line[512];
    int fd = SC(SYS_open, "/proc/cmdline", O_RDONLY, 0);
    int n = fd < 0 ? 0 : SC(SYS_read, fd, line, sizeof(line) - 1);
    SC(SYS_close, fd, 0, 0);

    for (int i = 0; i < n; ++i) {
        int k = 0;

        while (word[k] && line[i + k] == word[k])
            ++k;

        if (!word[k])
            return 1;
    }

    return 0;
}

void aq_main(void)
{
    static struct { char *dev, *opt; } no_args = {0, 0}, disk = {"/dev/hda", 0};

    aq_begin("fscache");

    SC(SYS_mknod, "/dev/hda", S_IFBLK | 0600, DEV(3, 0));
    check("mount ext2", aq_mount("ext2", "/mnt", &disk), 0);
    check("mount tmpfs", aq_mount("tmpfs", "/tmp", &no_args), 0);
    aq_mount("procfs", "/proc", &no_args);

    if (cmdline_has("fscache.pressure")) {
        pressure();
        aq_end();
    }

    for (unsigned i = 0; i < sizeof(buf); ++i)
        buf[i] = i * 7;

    struct numbers t = suite("tmpfs", "/tmp", 16ul << 20);
    struct numbers e = suite("ext2", "/mnt", 48ul << 20);

    check("ext2: a file read again comes from memory, 10 times as fast or more",
          e.again >= 10 * e.read, 1);
    check("tmpfs: written a piece at a time, not 100 times slower than read",
          t.write * 100 >= t.read, 1);

    aq_end();
}
