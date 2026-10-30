/** \file
 * The block cache: what the disks read and write, kept in 4 KiB blocks.
 *
 * Writes stay in the cache (write back) until `blkcache_flush()` writes
 * them to the disk: for `sync()`, before restarting or powering off, when
 * too much was written (`DIRTY_MAX`), or at the next use of the cache once
 * a write is old enough (`DIRTY_AGE`). Each sector of a block is either
 * valid (it has the disk's data, or newer) or not, and either dirty (newer
 * than the disk's) or not, so a write doesn't need to read the rest of its
 * block. When the cache is full, the clean block used least recently
 * makes room.
 *
 * Drivers use the cache with absolute sector numbers, after adding the
 * partition's offset, so a sector is cached once, whichever device node
 * it was read through. The blocks that a read needs and the cache doesn't
 * have are read with one command, and adjacent dirty blocks are written
 * with one.
 *
 * The device's transfers are always to and from the cache's own buffers,
 * never the caller's. The caller's buffer may be a process's, which isn't
 * mapped until it is touched, and mapping it may read the disk: the
 * driver would wait for itself. The caller's buffer is only used once its
 * pages are mapped.
 */

#include <core/system.h>
#include <core/string.h>
#include <core/arch.h>
#include <mm/mm.h>
#include <dev/blkcache.h>
#include <aquila/errno.h>

MALLOC_DEFINE(M_BLKCACHE, "blkcache", "block cache");

#define SECTOR_SIZE     512
#define BLK_SECTORS     8
#define BLK_SIZE        (BLK_SECTORS * SECTOR_SIZE)
#define BLK_ALL         0xFF    /* all the sectors of a block, as a mask */

/* 8 MiB, in one allocation */
#define CACHE_BLOCKS    2048
#define CACHE_BUCKETS   1024

/* the most blocks one command reads or writes */
#define RUN_MAX         16

/* the most dirty blocks before they are written, and how long a block may
 * stay dirty */
#define DIRTY_MAX       (CACHE_BLOCKS / 2)
#define DIRTY_AGE       5000000000ULL   /* ns */

/** A block of the cache. */
struct blk {
    void *owner;            /* the device it is of; NULL while the slot is free */
    uint64_t nr;            /* the block's number: its first sector / BLK_SECTORS */
    uint8_t valid, dirty;   /* which of its sectors are valid, and dirty */
    blkcache_io write;      /* how its dirty sectors get to the disk */

    struct blk *hnext;              /* the next block in its hash chain */
    struct blk *newer, *older;      /* the list from most to least recently used */
};

static struct blk blks[CACHE_BLOCKS];
static char *arena;

static struct blk *buckets[CACHE_BUCKETS];
static struct blk *newest, *oldest;
static size_t used, dirty;

/* when the oldest dirty block of the cache became dirty */
static uint64_t dirty_since;

/* how many writes there were: a read during which a write came doesn't
 * cache what it read, since that may be older than what was written */
static uint64_t writes;

/** Returns a block's data. */
static inline char *blk_data(struct blk *b)
{
    return arena + (b - blks) * BLK_SIZE;
}

/** Returns the mask of `n` sectors from sector `first` of a block. */
static inline uint8_t sector_mask(size_t first, size_t n)
{
    return ((1U << n) - 1) << first;
}

/** Returns the hash chain that block `nr` of `owner` is on. */
static inline struct blk **bucket(void *owner, uint64_t nr)
{
    return &buckets[(nr ^ ((uintptr_t) owner >> 4)) % CACHE_BUCKETS];
}

/** Returns block `nr` of `owner`, or NULL if it isn't cached. */
static struct blk *lookup(void *owner, uint64_t nr)
{
    for (struct blk *b = *bucket(owner, nr); b; b = b->hnext)
        if (b->owner == owner && b->nr == nr)
            return b;

    return NULL;
}

/** Takes a block off the list of blocks by use. */
static void lru_unlink(struct blk *b)
{
    if (b->newer) b->newer->older = b->older; else newest = b->older;
    if (b->older) b->older->newer = b->newer; else oldest = b->newer;
    b->newer = b->older = NULL;
}

/** Puts a block at the head of the list: the most recently used. */
static void lru_push(struct blk *b)
{
    b->older = newest;
    b->newer = NULL;

    if (newest) newest->newer = b; else oldest = b;
    newest = b;
}

/** Marks a block as just used. */
static void touch(struct blk *b)
{
    if (b != newest) {
        lru_unlink(b);
        lru_push(b);
    }
}

/** Takes a clean block out of the cache. */
static void evict(struct blk *b)
{
    struct blk **p = bucket(b->owner, b->nr);

    while (*p != b)
        p = &(*p)->hnext;

    *p = b->hnext;
    lru_unlink(b);
    b->owner = NULL;
    --used;
}

/** Marks sectors of a block dirty, to be written with `write`. */
static void make_dirty(struct blk *b, uint8_t mask, blkcache_io write)
{
    if (!b->dirty && !dirty++)
        dirty_since = arch_rtime_ns();

    b->dirty |= mask;
    b->write = write;
}

/** Marks sectors of a block clean. */
static void make_clean(struct blk *b, uint8_t mask)
{
    if (b->dirty && !(b->dirty &= ~mask))
        --dirty;
}

/** Takes a block for block `nr` of `owner`, with nothing valid in it: a
 * free slot, or the clean block used least recently. Returns NULL if
 * every block is dirty, or there is no memory for the cache. */
static struct blk *block_new(void *owner, uint64_t nr)
{
    if (!arena && !(arena = kmalloc(CACHE_BLOCKS * BLK_SIZE, &M_BLKCACHE, 0)))
        return NULL;

    struct blk *b = NULL;

    if (used < CACHE_BLOCKS) {
        for (size_t i = 0; i < CACHE_BLOCKS && !b; ++i)
            if (!blks[i].owner)
                b = &blks[i];
    } else {
        /* the clean block that was used least recently */
        for (struct blk *old = oldest; old && !b; old = old->newer)
            if (!old->dirty)
                b = old;

        if (!b)
            return NULL;

        evict(b);
    }

    b->owner = owner;
    b->nr = nr;
    b->valid = b->dirty = 0;
    b->hnext = *bucket(owner, nr);
    *bucket(owner, nr) = b;
    lru_push(b);
    ++used;

    return b;
}

/** Maps every page of a buffer, by touching them: a process's buffer
 * isn't mapped until it is used. It is done before the cache is looked
 * at, because mapping a page may read the disk and change the cache. */
static void prefault(void *buf, size_t size, int write)
{
    volatile char *p = buf, *end = (char *) buf + size;

    for (; p < end; p = (volatile char *) PAGE_ALIGN((uintptr_t) p + PAGE_SIZE)) {
        char c = *p;
        if (write) *p = c;
    }

    if (size) {
        char c = end[-1];
        if (write) end[-1] = c;
    }
}

/** Finds the sectors of block `nr` that are in the request `[sector,
 * sector + count)`: `n` of them, from sector `first` of the block, at
 * sector `off` of the request. */
static void overlap(uint64_t nr, uint64_t sector, size_t count,
                    size_t *first, size_t *n, size_t *off)
{
    uint64_t start = MAX(nr * BLK_SECTORS, sector);
    uint64_t end = MIN((nr + 1) * BLK_SECTORS, sector + count);

    *first = start - nr * BLK_SECTORS;
    *n = end - start;
    *off = start - sector;
}

/** Returns whether a block exists and all its sectors are dirty. */
static inline int full_dirty(struct blk *b)
{
    return b && b->dirty == BLK_ALL;
}

/** Writes the dirty sectors of `b` to the disk (with the wholly dirty
 * blocks right after it, in one command), and marks them clean. Returns 0
 * or a negative errno; sectors whose write failed stay dirty. */
static int flush_block(struct blk *b)
{
    void *owner = b->owner;
    blkcache_io write = b->write;
    uint64_t nr = b->nr;
    int err = 0;

    char *tmp = kmalloc(RUN_MAX * BLK_SIZE, &M_BLKCACHE, 0);
    if (!tmp) return -ENOMEM;

    if (full_dirty(b)) {
        /* a run of wholly dirty blocks, written with one command */
        size_t run = 0;
        struct blk *r;

        while (run < RUN_MAX && full_dirty(r = lookup(owner, nr + run)) && r->write == write) {
            memcpy(tmp + run * BLK_SIZE, blk_data(r), BLK_SIZE);
            make_clean(r, BLK_ALL);
            ++run;
        }

        if ((err = write(owner, nr * BLK_SECTORS, run * BLK_SECTORS, tmp)) < 0) {
            /* they aren't on the disk: dirty again, if still cached */
            for (size_t i = 0; i < run; ++i)
                if ((r = lookup(owner, nr + i)))
                    make_dirty(r, BLK_ALL, write);
        }
    } else {
        /* write each range of dirty sectors */
        uint8_t mask = b->dirty;

        for (size_t s = 0; s < BLK_SECTORS && !err; ) {
            if (!(mask & (1 << s))) {
                ++s;
                continue;
            }

            size_t n = 0;
            while (s + n < BLK_SECTORS && (mask & (1 << (s + n))))
                ++n;

            uint8_t m = sector_mask(s, n);
            memcpy(tmp, blk_data(b) + s * SECTOR_SIZE, n * SECTOR_SIZE);
            make_clean(b, m);

            if ((err = write(owner, nr * BLK_SECTORS + s, n, tmp)) < 0) {
                struct blk *r = lookup(owner, nr);
                if (r)
                    make_dirty(r, m, write);
            }

            /* b may have left the cache while the write slept */
            if (!(b = lookup(owner, nr)))
                break;

            s += n;
        }
    }

    kfree(tmp);
    return err < 0 ? err : 0;
}

/**
 * Writes every dirty block of the cache to its disk.
 *
 * @return 0, or a negative errno if a write failed
 */
int blkcache_flush(void)
{
    int err = 0;

    /* go in slot order; writing sleeps, so the cache may change meanwhile,
     * and each block is looked at again */
    for (size_t i = 0; i < CACHE_BLOCKS && dirty; ++i) {
        struct blk *b = &blks[i];

        if (!b->owner || !b->dirty)
            continue;

        /* start from the first block of its run of wholly dirty blocks */
        if (full_dirty(b)) {
            struct blk *prev;

            for (size_t k = 0; k < RUN_MAX && b->nr && full_dirty(prev = lookup(b->owner, b->nr - 1)) &&
                               prev->write == b->write; ++k)
                b = prev;
        }

        int e = flush_block(b);
        if (e)
            err = e;

        /* the slot may hold more dirty sectors now: look at it again */
        if (blks[i].owner && blks[i].dirty && !e)
            --i;
    }

    return err;
}

/**
 * Writes the dirty blocks to the disk if the oldest one is old enough
 * (`DIRTY_AGE`). The cache does it at its next use; the page cache's
 * flusher, every second, so that a write reaches the disk in a few
 * seconds even if nothing uses the disk after it.
 */
void blkcache_flush_old(void)
{
    static int flushing;

    if (dirty && !flushing && arch_rtime_ns() - dirty_since > DIRTY_AGE) {
        flushing = 1;
        blkcache_flush();
        flushing = 0;
    }
}

/**
 * Reads sectors of a disk through the cache. The sectors the cache
 * doesn't have are read with `read`, and cached.
 *
 * @param owner     the disk (the driver's, for its sectors)
 * @param sector    the first sector, absolute on the disk
 * @param count     how many sectors
 * @param buf       where to put them
 * @param nsectors  the disk's size in sectors, or 0 if it isn't known
 * @param read      how the driver reads sectors
 * @return 0, or a negative errno (`-EIO` past the disk's end)
 */
ssize_t blkcache_read(void *owner, uint64_t sector, size_t count, void *buf,
                      uint64_t nsectors, blkcache_io read)
{
    char *out = buf;
    uint64_t last = (sector + count - 1) / BLK_SECTORS;
    size_t first, n, off;

    if (!count)
        return 0;

    blkcache_flush_old();
    prefault(buf, count * SECTOR_SIZE, 1);

    for (uint64_t nr = sector / BLK_SECTORS; nr <= last; ) {
        struct blk *b = lookup(owner, nr);

        overlap(nr, sector, count, &first, &n, &off);
        uint8_t need = sector_mask(first, n);

        if (b && (b->valid & need) == need) {
            memcpy(out + off * SECTOR_SIZE, blk_data(b) + first * SECTOR_SIZE, n * SECTOR_SIZE);
            touch(b);
            ++nr;
            continue;
        }

        /* read the blocks from here on that miss sectors, at once */
        size_t run = 1;
        while (nr + run <= last && run < RUN_MAX) {
            struct blk *r = lookup(owner, nr + run);
            size_t f, k, o;

            overlap(nr + run, sector, count, &f, &k, &o);

            if (r && (r->valid & sector_mask(f, k)) == sector_mask(f, k))
                break;

            ++run;
        }

        uint64_t start = nr * BLK_SECTORS;
        size_t sectors = run * BLK_SECTORS;

        /* not past the end of the disk, if it is known */
        if (nsectors && start + sectors > nsectors)
            sectors = start < nsectors ? nsectors - start : 0;

        if (!sectors)
            return -EIO;

        char *tmp = kmalloc(sectors * SECTOR_SIZE, &M_BLKCACHE, 0);

        if (!tmp)
            return -ENOMEM;

        uint64_t w = writes;
        ssize_t err = read(owner, start, sectors, tmp);

        if (err < 0) {
            kfree(tmp);
            return err;
        }

        for (size_t i = 0; i < run; ++i) {
            size_t have = sectors > i * BLK_SECTORS ? MIN(BLK_SECTORS, sectors - i * BLK_SECTORS) : 0;
            char *data = tmp + i * BLK_SIZE;

            overlap(nr + i, sector, count, &first, &n, &off);

            if (first + n > have) {
                kfree(tmp);
                return -EIO;
            }

            /* if a block was written meanwhile, the cache has newer
             * sectors than the disk had, and only the others are filled
             * in. A block that wasn't cached is only cached if nothing
             * was written meanwhile. */
            struct blk *r = lookup(owner, nr + i);

            if (!r && w == writes && (r = block_new(owner, nr + i)))
                r->valid = 0;

            if (r) {
                for (size_t s = 0; s < have; ++s) {
                    if (!(r->valid & (1 << s))) {
                        memcpy(blk_data(r) + s * SECTOR_SIZE, data + s * SECTOR_SIZE, SECTOR_SIZE);
                        r->valid |= 1 << s;
                    }
                }

                memcpy(out + off * SECTOR_SIZE, blk_data(r) + first * SECTOR_SIZE, n * SECTOR_SIZE);
                touch(r);
            } else {
                memcpy(out + off * SECTOR_SIZE, data + first * SECTOR_SIZE, n * SECTOR_SIZE);
            }
        }

        kfree(tmp);
        nr += run;
    }

    return 0;
}

/**
 * Writes sectors of a disk through the cache: they stay dirty in it until
 * it is flushed. If the cache has no room even after flushing, they go
 * straight to the disk.
 *
 * @param owner   the disk (the driver's, for its sectors)
 * @param sector  the first sector, absolute on the disk
 * @param count   how many sectors
 * @param buf     the sectors' data
 * @param write   how the driver writes sectors
 * @return 0, or a negative errno
 */
ssize_t blkcache_write(void *owner, uint64_t sector, size_t count, void *buf,
                       blkcache_io write)
{
    char *in = buf;
    size_t first, n, off;
    ssize_t err = 0;

    if (!count)
        return 0;

    blkcache_flush_old();
    prefault(buf, count * SECTOR_SIZE, 0);

    uint64_t last = (sector + count - 1) / BLK_SECTORS;

    for (uint64_t nr = sector / BLK_SECTORS; nr <= last; ++nr) {
        overlap(nr, sector, count, &first, &n, &off);

        struct blk *b = lookup(owner, nr);

        if (!b && !(b = block_new(owner, nr))) {
            /* every block is dirty: write them, and then there is room */
            blkcache_flush();
            b = lookup(owner, nr);

            if (!b && !(b = block_new(owner, nr))) {
                /* no memory for the cache: write straight to the disk */
                char *tmp = kmalloc(n * SECTOR_SIZE, &M_BLKCACHE, 0);

                if (!tmp)
                    return -ENOMEM;

                memcpy(tmp, in + off * SECTOR_SIZE, n * SECTOR_SIZE);
                err = write(owner, nr * BLK_SECTORS + first, n, tmp);
                kfree(tmp);

                if (err < 0)
                    return err;

                continue;
            }
        }

        uint8_t m = sector_mask(first, n);

        memcpy(blk_data(b) + first * SECTOR_SIZE, in + off * SECTOR_SIZE, n * SECTOR_SIZE);
        b->valid |= m;
        make_dirty(b, m, write);
        touch(b);
    }

    ++writes;

    if (dirty >= DIRTY_MAX)
        err = blkcache_flush();

    return err < 0 ? err : 0;
}
