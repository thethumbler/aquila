/*
 * ext2 write path
 *
 * Mounts the ext2 image attached as the first ATA disk, writes a file
 * large enough to need singly and doubly indirect blocks (1K blocks),
 * appends to it after reopening, and reads everything back. Then
 * directories that grow past a block, removing, renaming and linking,
 * and truncating; a sparse file with a block past the doubly indirect
 * ones (triply indirect), kept, and another truncated away; the disk has
 * no partition table: its first partition isn't there (it isn't the whole
 * disk). Then the page cache: what reads see after writes, truncations
 * and mappings, two processes reading a file, an inode reused after its
 * file was unlinked, a program run from where it was just copied.
 * tests/run.sh then checks the image on the host with
 * e2fsck, compares the file contents with the same pattern and lists
 * what is left.
 */

#include "../lib/aqtest.h"

#include <aquila/dirent.h>
#include <aquila/mman.h>

/* > 12 direct + 256 singly indirect 1K blocks, reaches doubly indirect */
#define FIRST   300000u
#define EXTRA   5000u

/* past 12 + 256 + 65536 1K blocks: triply indirect */
#define HUGE_AT (70u << 20)

static char buf[8192];
static char st[128];

#define ST ((struct aquila_stat *) st)

/* "<prefix>NN" */
static char *name(const char *prefix, int n)
{
    static char s[32];
    int i = 0;

    while (*prefix)
        s[i++] = *prefix++;

    s[i++] = '0' + n / 10;
    s[i++] = '0' + n % 10;
    s[i] = 0;
    return s;
}

/* number of entries of a directory, without . and .. */
static int entries(const char *path)
{
    struct dirent d;
    int fd = SC(SYS_open, path, O_RDONLY, 0), n = 0;

    while (SC(SYS_readdir, fd, &d, 0) > 0)
        n += !(d.d_name[0] == '.' && (!d.d_name[1] || (d.d_name[1] == '.' && !d.d_name[2])));

    SC(SYS_close, fd, 0, 0);
    return n;
}

/* must match the pattern in tests/run.sh */
static unsigned char pat(u32 off)
{
    return (unsigned char) (off * 7 + (off >> 9));
}

static int write_pattern(int fd, u32 start, u32 len)
{
    while (len) {
        u32 n = len > sizeof(buf) ? sizeof(buf) : len;

        for (u32 i = 0; i < n; ++i)
            buf[i] = pat(start + i);

        int w = SC(SYS_write, fd, buf, n);
        if (w != (int) n)
            return w < 0 ? w : -1;

        start += n;
        len   -= n;
    }

    return 0;
}

/* return the offset of the first mismatch, or -1 if all bytes match */
static int verify(int fd, u32 len)
{
    u32 off = 0;

    SC(SYS_lseek, fd, 0, 0);

    while (off < len) {
        u32 n = len - off > sizeof(buf) ? sizeof(buf) : len - off;
        int r = SC(SYS_read, fd, buf, n);

        if (r <= 0)
            return off;

        for (int i = 0; i < r; ++i)
            if ((unsigned char) buf[i] != pat(off + i))
                return off + i;

        off += r;
    }

    return -1;
}

/* a number in /proc/meminfo, after "key: ", or -1 if it isn't there */
static long meminfo(const char *key)
{
    static char mi[512];
    int fd = SC(SYS_open, "/proc/meminfo", O_RDONLY, 0);
    int n = SC(SYS_read, fd, mi, sizeof(mi) - 1);
    SC(SYS_close, fd, 0, 0);

    for (int i = 0; i < n; ++i) {
        int k = 0;

        while (key[k] && mi[i + k] == key[k])
            ++k;

        if (!key[k]) {
            long v = 0;

            for (char *c = &mi[i + k]; *c >= '0' && *c <= '9'; ++c)
                v = v * 10 + (*c - '0');

            return v;
        }
    }

    return -1;
}

/* whether `fd` reads `len` bytes of `c` from `off` */
static int reads_as(int fd, u32 off, u32 len, char c)
{
    char b[64];

    while (len) {
        u32 n = len > sizeof(b) ? sizeof(b) : len;

        if (SC5(SYS_pread, fd, b, n, off, 0) != (long) n)
            return 0;

        for (u32 i = 0; i < n; ++i)
            if (b[i] != c)
                return 0;

        off += n;
        len -= n;
    }

    return 1;
}

static void fill(int fd, u32 off, u32 len, char c)
{
    for (u32 i = 0; i < sizeof(buf); ++i)
        buf[i] = c;

    while (len) {
        u32 n = len > sizeof(buf) ? sizeof(buf) : len;
        SC5(SYS_pwrite, fd, buf, n, off, 0);
        off += n;
        len -= n;
    }
}

/* what reads see, through the page cache */
static void page_cache(void)
{
    static struct { char *dev, *opt; } no_args = {0, 0};
    aq_mount("procfs", "/proc", &no_args);

    /* a file read is kept: Cached grows */
    int a = SC(SYS_open, "/mnt/pc", O_CREAT | O_RDWR | O_TRUNC, 0644);
    fill(a, 0, 3 * 4096, 'a');
    long cached = meminfo("Cached: ");
    check("page cache: a file read back", reads_as(a, 0, 3 * 4096, 'a'), 1);
    check("page cache: Cached grows", meminfo("Cached: ") >= cached + 12, 1);

    /* a write through another descriptor: the next read sees it */
    int b = SC(SYS_open, "/mnt/pc", O_RDWR, 0);
    fill(b, 4096 + 10, 3, 'X');
    check("page cache: a write seen by another descriptor's read",
          reads_as(a, 4096 + 10, 3, 'X') && reads_as(a, 4096 + 9, 1, 'a') && reads_as(a, 4096 + 13, 1, 'a'), 1);

    /* a mapping and read() see the same, after a write */
    void *m = 0;
    struct aquila_mmap_args ma = {0, 3 * 4096, PROT_READ, MAP_PRIVATE, a, 0};
    check("page cache: mmap", SC(SYS_mmap, &ma, &m, 0), 0);
    fill(b, 2 * 4096, 1, 'M');
    check("page cache: a write seen by the mapping and by read()",
          ((volatile char *) m)[2 * 4096] == 'M' && reads_as(a, 2 * 4096, 1, 'M'), 1);
    SC(SYS_munmap, m, 3 * 4096, 0);

    /* truncated while cached: shorter, then longer, zeros */
    SC(SYS_ftruncate, a, 5000, 0);
    check("page cache: truncated, the end", SC5(SYS_pread, a, buf, 100, 4990, 0), 10);
    SC(SYS_ftruncate, a, 3 * 4096, 0);
    check("page cache: truncated then grown, zeros", reads_as(a, 5000, 3 * 4096 - 5000, 0), 1);

    /* two processes read the same file */
    int pid = SC(SYS_fork, 0, 0, 0);
    if (!pid)
        SC(SYS_exit, reads_as(a, 0, 4096 + 10, 'a') && reads_as(a, 4096 + 10, 3, 'X'), 0, 0);
    int ws = 0;
    SC(SYS_waitpid, pid, &ws, 0);
    check("page cache: another process reads the same", ws >> 8, 1);
    SC(SYS_close, a, 0, 0);
    SC(SYS_close, b, 0, 0);

    /* an unlinked file's inode reused: the new file isn't the old one's
     * pages (a hole where the old file had data reads zeros) */
    a = SC(SYS_open, "/mnt/old", O_CREAT | O_RDWR | O_TRUNC, 0644);
    fill(a, 0, 2 * 4096, 'o');
    reads_as(a, 0, 2 * 4096, 'o');
    SC(SYS_fstat, a, st, 0);
    unsigned long ino = ST->st_ino;
    SC(SYS_close, a, 0, 0);
    SC(SYS_unlink, "/mnt/old", 0, 0);
    a = SC(SYS_open, "/mnt/new", O_CREAT | O_RDWR | O_TRUNC, 0644);
    fill(a, 4096 + 100, 1, 'n');
    SC(SYS_fstat, a, st, 0);
    check("page cache: the inode is reused", ST->st_ino, ino);
    check("page cache: a reused inode reads the new file", reads_as(a, 0, 4096 + 100, 0) &&
          reads_as(a, 4096 + 100, 1, 'n'), 1);
    SC(SYS_close, a, 0, 0);
    SC(SYS_unlink, "/mnt/new", 0, 0);

    /* a program copied onto the disk runs from there */
    int in = SC(SYS_open, "/init", O_RDONLY, 0), out = SC(SYS_open, "/mnt/prog", O_CREAT | O_WRONLY | O_TRUNC, 0755);
    long r;
    while ((r = SC(SYS_read, in, buf, sizeof(buf))) > 0)
        SC(SYS_write, out, buf, r);
    SC(SYS_close, in, 0, 0);
    SC(SYS_close, out, 0, 0);
    pid = SC(SYS_fork, 0, 0, 0);
    if (!pid) {
        char *argv[] = {"/mnt/prog", "page-cache-exec", 0};
        SC(SYS_execve, "/mnt/prog", argv, 0);
        SC(SYS_exit, 1, 0, 0);
    }
    SC(SYS_waitpid, pid, &ws, 0);
    check("page cache: a program run where it was copied", ws >> 8, 42);
    SC(SYS_unlink, "/mnt/prog", 0, 0);
    SC(SYS_unlink, "/mnt/pc", 0, 0);
}

void aq_main(void)
{
    struct { char *dev, *opt; } args = {"/dev/hda", 0};

    /* run from the disk by page_cache() */
    if (aq_argc == 2) {
        const char *want = "page-cache-exec", *arg = aq_argv[1];

        while (*want && *arg == *want)
            ++arg, ++want;

        if (!*want && !*arg)
            SC(SYS_exit, 42, 0, 0);
    }

    aq_begin("ext2");

    check("mknod /dev/hda", SC(SYS_mknod, "/dev/hda", S_IFBLK | 0644, DEV(3, 0)), 0);
    check("mount ext2", aq_mount("ext2", "/mnt", &args), 0);

    int fd = SC(SYS_open, "/mnt/big", O_RDWR | O_CREAT, 0644);
    check("create file", fd >= 0, 1);
    check("write through indirect blocks", write_pattern(fd, 0, FIRST), 0);
    check("close", SC(SYS_close, fd, 0, 0), 0);

    /* append after reopening: used to wipe the inode block pointers */
    fd = SC(SYS_open, "/mnt/big", O_RDWR, 0);
    check("reopen", fd >= 0, 1);
    check("seek to end", SC(SYS_lseek, fd, FIRST, 0), FIRST);
    check("append", write_pattern(fd, FIRST, EXTRA), 0);
    check("read back", verify(fd, FIRST + EXTRA), -1);
    check("close", SC(SYS_close, fd, 0, 0), 0);

    fd = SC(SYS_open, "/mnt/small", O_RDWR | O_CREAT, 0644);
    check("small file", SC(SYS_write, fd, "hello aquila\n", 13), 13);
    SC(SYS_close, fd, 0, 0);

    /* device nodes keep their numbers (the host reads devnode's), and
     * one removed frees no blocks (e2fsck) */
    check("mknod on ext2", SC(SYS_mknod, "/mnt/devnode", S_IFBLK | 0644, DEV(3, 1)), 0);
    SC(SYS_stat, "/mnt/devnode", st, 0);
    check("device node's number", ST->st_rdev, DEV(3, 1));
    check("mknod another", SC(SYS_mknod, "/mnt/gone", S_IFCHR | 0644, DEV(4, 64)), 0);
    SC(SYS_stat, "/mnt/gone", st, 0);
    check("its number", ST->st_rdev, DEV(4, 64));
    check("unlink a device node", SC(SYS_unlink, "/mnt/gone", 0, 0), 0);

    /* a directory that needs more than one block (16-byte entries) */
    SC(SYS_stat, "/mnt", st, 0);
    int root_links = ST->st_nlink;
    check("mkdir", SC(SYS_mkdir, "/mnt/d", 0755, 0), 0);
    SC(SYS_stat, "/mnt", st, 0);
    check("parent links +1", ST->st_nlink, root_links + 1);
    int made = 0;
    for (int i = 0; i < 99; ++i) {
        char path[32] = "/mnt/d/";
        char *n = name("file-number-", i);
        for (int j = 0; n[j]; ++j) path[7 + j] = n[j];
        fd = SC(SYS_open, path, O_CREAT | O_WRONLY, 0644);
        made += fd >= 0;
        SC(SYS_close, fd, 0, 0);
    }
    check("99 files", made, 99);
    check("all listed", entries("/mnt/d"), 99);
    SC(SYS_stat, "/mnt/d", st, 0);
    check("directory grew", ST->st_size > 1024, 1);
    check("rmdir not empty", SC(SYS_rmdir, "/mnt/d", 0, 0), -ENOTEMPTY);
    int removed = 0;
    for (int i = 0; i < 99; i += 2) {
        char path[32] = "/mnt/d/";
        char *n = name("file-number-", i);
        for (int j = 0; n[j]; ++j) path[7 + j] = n[j];
        removed += SC(SYS_unlink, path, 0, 0) == 0;
    }
    check("unlinked 50", removed, 50);
    check("49 left", entries("/mnt/d"), 49);
    check("removed one gone", SC(SYS_stat, "/mnt/d/file-number-00", st, 0), -ENOENT);
    check("kept one there", SC(SYS_stat, "/mnt/d/file-number-01", st, 0), 0);
    for (int i = 1; i < 99; i += 2) {
        char path[32] = "/mnt/d/";
        char *n = name("file-number-", i);
        for (int j = 0; n[j]; ++j) path[7 + j] = n[j];
        SC(SYS_unlink, path, 0, 0);
    }
    check("empty again", entries("/mnt/d"), 0);
    check("rmdir", SC(SYS_rmdir, "/mnt/d", 0, 0), 0);
    SC(SYS_stat, "/mnt", st, 0);
    check("parent links back", ST->st_nlink, root_links);

    /* rename, also of a directory to another parent */
    SC(SYS_mkdir, "/mnt/a", 0755, 0);
    SC(SYS_mkdir, "/mnt/b", 0755, 0);
    SC(SYS_mkdir, "/mnt/a/sub", 0755, 0);
    fd = SC(SYS_open, "/mnt/a/sub/f", O_CREAT | O_WRONLY, 0644);
    SC(SYS_write, fd, "moved", 5);
    SC(SYS_close, fd, 0, 0);
    check("rename a directory", SC5(SYS_renameat, AT_FDCWD, "/mnt/a/sub", AT_FDCWD, "/mnt/b/sub", 0), 0);
    check("moved with its contents", SC(SYS_stat, "/mnt/b/sub/f", st, 0), 0);
    check(".. follows", SC(SYS_stat, "/mnt/b/sub/../sub/f", st, 0), 0);
    SC(SYS_stat, "/mnt/a", st, 0);
    check("old parent links", ST->st_nlink, 2);
    SC(SYS_stat, "/mnt/b", st, 0);
    check("new parent links", ST->st_nlink, 3);
    check("rename a file", SC5(SYS_renameat, AT_FDCWD, "/mnt/b/sub/f", AT_FDCWD, "/mnt/renamed", 0), 0);
    fd = SC(SYS_open, "/mnt/small", O_RDONLY, 0);
    SC(SYS_close, fd, 0, 0);
    check("rename over a file", SC5(SYS_renameat, AT_FDCWD, "/mnt/renamed", AT_FDCWD, "/mnt/small", 0), 0);
    SC(SYS_stat, "/mnt/small", st, 0);
    check("replaced", ST->st_size, 5);
    check("rmdir moved", SC(SYS_rmdir, "/mnt/b/sub", 0, 0), 0);

    /* links */
    check("link", SC(SYS_link, "/mnt/small", "/mnt/a/hard", 0), 0);
    SC(SYS_stat, "/mnt/small", st, 0);
    check("2 links", ST->st_nlink, 2);
    check("unlink one name", SC(SYS_unlink, "/mnt/small", 0, 0), 0);
    SC(SYS_stat, "/mnt/a/hard", st, 0);
    check("1 link", ST->st_nlink, 1);
    check("symlink", SC(SYS_symlinkat, "a/hard", AT_FDCWD, "/mnt/sym"), 0);
    char target[16] = {0};
    check("readlink", SC5(SYS_readlinkat, AT_FDCWD, "/mnt/sym", target, sizeof(target), 0), 6);
    fd = SC(SYS_open, "/mnt/sym", O_RDONLY, 0);
    check("read through symlink", SC(SYS_read, fd, buf, 16), 5);
    SC(SYS_close, fd, 0, 0);

    /* an unlinked file stays readable while open */
    fd = SC(SYS_open, "/mnt/gone", O_CREAT | O_RDWR, 0644);
    write_pattern(fd, 0, 20000);
    check("unlink open file", SC(SYS_unlink, "/mnt/gone", 0, 0), 0);
    check("still readable", verify(fd, 20000), -1);
    SC(SYS_close, fd, 0, 0);

    /* truncating frees blocks, growing again reads zeros */
    fd = SC(SYS_open, "/mnt/trunc", O_CREAT | O_RDWR, 0644);
    write_pattern(fd, 0, 100000);
    check("truncate", SC(SYS_ftruncate, fd, 1000, 0), 0);
    SC(SYS_fstat, fd, st, 0);
    check("size after truncate", ST->st_size, 1000);
    check("kept part", verify(fd, 1000), -1);
    SC(SYS_ftruncate, fd, 3000, 0);
    SC(SYS_lseek, fd, 1000, 0);
    int zeros = SC(SYS_read, fd, buf, 2000) == 2000;
    for (int i = 0; i < 2000; ++i)
        zeros &= buf[i] == 0;
    check("grown part reads zeros", zeros, 1);
    SC(SYS_close, fd, 0, 0);

    /* past the doubly indirect blocks (12 + 256 + 65536 KiB): triply
     * indirect, a sparse file; the hole reads zeros */
    fd = SC(SYS_open, "/mnt/huge", O_CREAT | O_RDWR, 0644);
    SC(SYS_lseek, fd, HUGE_AT, 0);
    check("write past the doubly indirect blocks", write_pattern(fd, HUGE_AT, 4096), 0);
    SC(SYS_fstat, fd, st, 0);
    check("its size", ST->st_size, HUGE_AT + 4096);
    SC(SYS_lseek, fd, HUGE_AT, 0);
    int same = SC(SYS_read, fd, buf, 4096) == 4096;
    for (u32 i = 0; i < 4096; ++i)
        same &= (unsigned char) buf[i] == pat(HUGE_AT + i);
    check("read back, triply indirect", same, 1);
    SC(SYS_lseek, fd, HUGE_AT - 8192, 0);
    zeros = SC(SYS_read, fd, buf, 8192) == 8192;
    for (int i = 0; i < 8192; ++i)
        zeros &= buf[i] == 0;
    check("the hole before reads zeros", zeros, 1);
    SC(SYS_close, fd, 0, 0);

    /* and freed: truncated to nothing (e2fsck counts the free blocks) */
    fd = SC(SYS_open, "/mnt/huge2", O_CREAT | O_RDWR, 0644);
    SC(SYS_lseek, fd, HUGE_AT, 0);
    write_pattern(fd, HUGE_AT, 4096);
    check("truncate a triply indirect file", SC(SYS_ftruncate, fd, 0, 0), 0);
    SC(SYS_fstat, fd, st, 0);
    check("no blocks left", ST->st_blocks, 0);
    SC(SYS_close, fd, 0, 0);
    check("unlink it", SC(SYS_unlink, "/mnt/huge2", 0, 0), 0);

    /* no partition table: no first partition (not the whole disk from
     * sector 0) */
    SC(SYS_mknod, "/dev/hda1", S_IFBLK | 0644, DEV(3, 1));
    fd = SC(SYS_open, "/dev/hda1", O_RDONLY, 0);
    check("a partition the disk doesn't have", fd >= 0 ? SC(SYS_read, fd, buf, 512) : fd, -ENXIO);
    SC(SYS_close, fd, 0, 0);

    page_cache();

    /* the disk has what was written (the host checks it) */
    check("sync", SC(SYS_sync, 0, 0, 0), 0);

    aq_end();
}
