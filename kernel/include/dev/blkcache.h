#ifndef _DEV_BLKCACHE_H
#define _DEV_BLKCACHE_H

#include <core/system.h>

/* The block cache (dev/blkcache.c): recently used disk sectors, kept in
 * memory, written back on sync. */

/* Reads or writes `count` sectors from `sector` of the owner's device. */
typedef ssize_t (*blkcache_io)(void *owner, uint64_t sector, size_t count, void *buf);

/* Reads sectors through the cache, or writes them into it. */
ssize_t blkcache_read(void *owner, uint64_t sector, size_t count, void *buf,
                      uint64_t nsectors, blkcache_io read);
ssize_t blkcache_write(void *owner, uint64_t sector, size_t count, void *buf,
                       blkcache_io write);

/* Writes what was written to the cache to the disks. */
int blkcache_flush(void);
void blkcache_flush_old(void);

#endif /* ! _DEV_BLKCACHE_H */
