#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>

/* PROT_* and MAP_* flags, shared with the kernel */
#include <aquila/mman.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t off);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);

#endif /* ! _SYS_MMAN_H */
