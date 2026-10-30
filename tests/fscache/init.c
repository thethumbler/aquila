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

void aq_main(void)
{
    static struct { char *dev, *opt; } no_args = {0, 0}, disk = {"/dev/hda", 0};

    aq_begin("fscache");

    SC(SYS_mknod, "/dev/hda", S_IFBLK | 0600, DEV(3, 0));
    check("mount ext2", aq_mount("ext2", "/mnt", &disk), 0);
    check("mount tmpfs", aq_mount("tmpfs", "/tmp", &no_args), 0);

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
