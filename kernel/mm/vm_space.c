/** \file
 * Address spaces: a process's mappings (map entries), kept in address
 * order, and its page tables (`pmap`).
 */

#include <core/system.h>
#include <core/arch.h>

#include <mm/pmap.h>
#include <mm/vm.h>

#include <ds/queue.h>

/**
 * Adds a map entry to an address space, in address order.
 *
 * If the entry's `base` is 0, the entry is placed at the highest free gap
 * between mappings that it fits in. The entry always goes before an
 * existing one (the stack's, at the top of user space): an entry past the
 * last one can't be added.
 *
 * @param vm_space  the address space
 * @param vm_entry  the entry, with its `base` (or 0) and `size` set
 * @return 0, `-EINVAL` if either is NULL, or `-ENOMEM` if there is no
 *         room for it, or no memory for its list node
 */
int vm_space_insert(struct vm_space *vm_space, struct vm_entry *vm_entry)
{
    if (!vm_space || !vm_entry)
        return -EINVAL;

    struct queue *queue = &vm_space->vm_entries;

    int alloc = !vm_entry->base;

    uintptr_t end = vm_entry->base + vm_entry->size;

    struct qnode *cur = NULL;
    uintptr_t prev_end = 0;

    if (alloc) {
        /* find the highest gap that the entry fits in */
        queue_for (node, queue) {
            struct vm_entry *cur_vm_entry = (struct vm_entry *) node->value;

            if (cur_vm_entry->base - prev_end >= vm_entry->size) {
                vm_entry->base = cur_vm_entry->base - vm_entry->size;
                end = vm_entry->base + vm_entry->size;
            }

            prev_end = cur_vm_entry->base + cur_vm_entry->size;
        }
    }

    prev_end = 0;

    queue_for (node, queue) {
        struct vm_entry *cur_vm_entry = (struct vm_entry *) node->value;

        if (vm_entry->base && cur_vm_entry->base >= end && prev_end <= vm_entry->base) {
            cur = node;
            break;
        }

        prev_end = cur_vm_entry->base + cur_vm_entry->size;
    }

    if (!cur)
        return -ENOMEM;

    struct qnode *node = kmalloc(sizeof(struct qnode), &M_QNODE, M_ZERO);

    if (!node)
        return -ENOMEM;

    node->value = vm_entry;
    node->next = cur;
    node->prev = cur->prev;

    if (cur->prev)
        cur->prev->next = node;
    else
        queue->head = node;

    cur->prev  = node;
    vm_entry->qnode = node;
    ++queue->count;

    return 0;
}

/**
 * Finds the map entry that an address is in.
 *
 * @param vm_space  the address space
 * @param vaddr     the address
 * @return the entry, or NULL if no mapping covers the address
 */
struct vm_entry *vm_space_find(struct vm_space *vm_space, vaddr_t vaddr)
{
    if (!vm_space) return NULL;

    vaddr = PAGE_ALIGN(vaddr);

    struct queue *vm_entries = &vm_space->vm_entries;

    queue_for (node, vm_entries) {
        struct vm_entry *vm_entry = node->value;
        vaddr_t vm_end = vm_entry->base + vm_entry->size;

        if (vaddr >= vm_entry->base && vaddr < vm_end)
            return vm_entry;
    }

    return NULL;
}

/**
 * Splits a map entry in two at an address, for `mprotect()` and
 * `munmap()` on part of a mapping: the entry keeps what is below `addr`,
 * and a new entry after it in the list has the rest, with the same
 * permissions and object. The pages the anon has at or past `addr` move
 * to an anon of the new entry's own. An anon that a fork shares is
 * copied first, as the first write to it would (`mm/fault.c`), so that
 * the other process's mapping doesn't change. The page tables don't
 * change.
 *
 * @param vm_space  the address space
 * @param vm_entry  the entry
 * @param addr      where to split it, on a page, past its start and
 *                  before its end
 * @return 0, `-EINVAL` for such an address, or `-ENOMEM` if there was no
 *         memory for the new entry; the entry is as it was then (but for
 *         its anon, copied maybe)
 */
int vm_space_split(struct vm_space *vm_space, struct vm_entry *vm_entry, vaddr_t addr)
{
    if ((addr & PAGE_MASK) || addr <= vm_entry->base || addr - vm_entry->base >= vm_entry->size)
        return -EINVAL;

    size_t delta = addr - vm_entry->base;

    struct vm_entry *right = vm_entry_new();
    struct qnode *node = kmalloc(sizeof(struct qnode), &M_QNODE, M_ZERO);
    struct vm_anon *vm_anon = vm_entry->vm_anon, *right_anon = NULL;

    if (!right || !node)
        goto nomem;

    if (vm_anon) {
        /* the anon is shared with another process: this mapping gets its
         * own, to split */
        if (vm_anon->ref > 1) {
            struct vm_anon *copy = vm_anon_copy(vm_anon);

            if (!copy)
                goto nomem;

            vm_anon_decref(vm_anon);
            vm_entry->vm_anon = vm_anon = copy;
        }

        /* the anon is keyed by the offset in the object */
        if (!(right_anon = vm_anon_split(vm_anon, vm_entry->off + delta)))
            goto nomem;
    }

    *right = *vm_entry;
    right->base    = addr;
    right->size    = vm_entry->size - delta;
    right->off     = vm_entry->off + delta;
    right->paddr   = vm_entry->paddr ? vm_entry->paddr + delta : 0;
    right->vm_anon = right_anon;
    right->qnode   = node;

    right->vm_space = vm_space;

    if (right->vm_object)
        vm_object_map(right->vm_object, right);

    vm_entry->size = delta;

    /* the new entry goes right after the old one */
    struct qnode *prev = vm_entry->qnode;

    node->value = right;
    node->prev  = prev;
    node->next  = prev->next;

    if (prev->next)
        prev->next->prev = node;
    else
        vm_space->vm_entries.tail = node;

    prev->next = node;
    vm_space->vm_entries.count++;

    return 0;

nomem:
    kfree(right);
    kfree(node);
    return -ENOMEM;
}

/** Splits the entries of an address space that cross `start` or `end`,
 * so that every entry is either inside the range or outside it. Returns
 * 0 or `-ENOMEM`. */
static int vm_space_cut(struct vm_space *vm_space, vaddr_t start, vaddr_t end)
{
    struct vm_entry *vm_entry;
    int err;

    if ((vm_entry = vm_space_find(vm_space, start)) && vm_entry->base < start &&
            (err = vm_space_split(vm_space, vm_entry, start)))
        return err;

    if ((vm_entry = vm_space_find(vm_space, end)) && vm_entry->base < end &&
            (err = vm_space_split(vm_space, vm_entry, end)))
        return err;

    return 0;
}

/** Says whether every page of a range of an address space is mapped. */
static int vm_space_covers(struct vm_space *vm_space, vaddr_t start, vaddr_t end)
{
    vaddr_t at = start;

    /* the entries are in address order: each must start where the
     * range is mapped up to */
    queue_for (node, &vm_space->vm_entries) {
        struct vm_entry *vm_entry = node->value;
        vaddr_t vm_end = vm_entry->base + vm_entry->size;

        if (!vm_entry->size || vm_end <= at)
            continue;

        if (vm_entry->base > at)
            return 0;

        at = vm_end;

        if (at >= end)
            return 1;
    }

    return 0;
}

/**
 * Changes the permissions of a range of an address space, for
 * `mprotect()`. The entries that cross either end of the range are split
 * there first.
 *
 * The pages already mapped get the new permissions, but never writable:
 * a write to one faults, and the fault makes the page writable if the
 * process has it alone, or copies it (`mm/fault.c`), as for a page a fork
 * shared. Without any permission, the pages are unmapped instead, and
 * stay with the anon. Device memory (`paddr`), which no fault brings in,
 * is mapped again with the permissions as they are.
 *
 * @param vm_space  the address space
 * @param start     the range's start, on a page
 * @param end       its end, on a page
 * @param perm      the user permissions (`VM_UR`, `VM_UW`, `VM_UX`)
 * @return 0, `-ENOMEM` if a page of the range isn't mapped (nothing
 *         changes then), or if there was no memory to split an entry or
 *         for a page table of device memory, or `-EACCES` to make a
 *         file's shared mapping writable
 */
int vm_space_protect(struct vm_space *vm_space, vaddr_t start, vaddr_t end, uint32_t perm)
{
    int err;

    if (!vm_space_covers(vm_space, start, end))
        return -ENOMEM;

    /* a file's shared mapping can't be written to: what's written isn't
     * written back to the file yet */
    queue_for (node, &vm_space->vm_entries) {
        struct vm_entry *vm_entry = node->value;
        vaddr_t eva = vm_entry->base + vm_entry->size;

        if (vm_entry->size && eva > start && vm_entry->base < end && (perm & VM_UW) &&
            (vm_entry->flags & VM_SHARED) && vm_entry->vm_object && !vm_entry->paddr)
            return -EACCES;
    }

    if ((err = vm_space_cut(vm_space, start, end)))
        return err;

    queue_for (node, &vm_space->vm_entries) {
        struct vm_entry *vm_entry = node->value;
        vaddr_t sva = vm_entry->base, eva = sva + vm_entry->size;

        if (!vm_entry->size || sva < start || eva > end)
            continue;

        vm_entry->flags = (vm_entry->flags & ~VM_PERM) | perm;

        if (vm_entry->paddr) {
            /* device memory doesn't come in on faults: it's mapped again
             * whole, with the new permissions */
            pmap_remove(vm_space->pmap, sva, eva);

            if (perm && (err = vm_map(vm_space, vm_entry)))
                return err;
        } else if (!perm) {
            pmap_remove(vm_space->pmap, sva, eva);
        } else {
            pmap_protect(vm_space->pmap, sva, eva, perm & ~VM_UW);
        }
    }

    return 0;
}

/**
 * Removes the mappings of a range of an address space, for `munmap()`.
 * The entries that cross either end of the range are split there first,
 * and those inside it go, with the pages that only they had. Parts of the
 * range that aren't mapped are left alone.
 *
 * @param vm_space  the address space
 * @param start     the range's start, on a page
 * @param end       its end, on a page
 * @return 0, or `-ENOMEM` if there was no memory to split an entry
 *         (nothing is removed then)
 */
int vm_space_remove(struct vm_space *vm_space, vaddr_t start, vaddr_t end)
{
    int err;

    if ((err = vm_space_cut(vm_space, start, end)))
        return err;

    struct qnode *node = vm_space->vm_entries.head;

    while (node) {
        struct qnode *next = node->next;
        struct vm_entry *vm_entry = node->value;
        vaddr_t sva = vm_entry->base, eva = sva + vm_entry->size;

        if (vm_entry->size && sva >= start && eva <= end) {
            queue_node_remove(&vm_space->vm_entries, node);
            pmap_remove(vm_space->pmap, sva, eva);
            vm_entry_destroy(vm_entry);
            kfree(vm_entry);
        }

        node = next;
    }

    return 0;
}

/**
 * Removes all the mappings of an address space: its entries are released
 * and freed, and its page tables emptied.
 *
 * @param vm_space  the address space (NULL does nothing)
 */
void vm_space_destroy(struct vm_space *vm_space)
{
    if (!vm_space)
        return;

    struct vm_entry *vm_entry = NULL;

    while ((vm_entry = dequeue(&vm_space->vm_entries))) {
        vm_entry_destroy(vm_entry);
        kfree(vm_entry);
    }

    pmap_remove_all(vm_space->pmap);
}

/**
 * Copies an address space's mappings into another, for `fork()`. The two
 * share the pages, copy on write: writable private mappings become
 * read-only in `src`, and their anons are marked to be copied, so that
 * the first write to a page gives the writer its own copy
 * (`mm/fault.c`).
 *
 * @param src  the parent's address space
 * @param dst  the child's address space, empty
 * @return 0, `-EINVAL` if either is NULL, or `-ENOMEM` if there was no
 *         memory for an entry; what was copied stays in `dst`, for the
 *         caller to destroy
 */
int vm_space_fork(struct vm_space *src, struct vm_space *dst)
{
    if (!src || !dst)
        return -EINVAL;

    queue_for (node, &src->vm_entries) {
        struct vm_entry *s_entry = node->value;
        struct vm_entry *d_entry = kmalloc(sizeof(struct vm_entry), &M_VM_ENTRY, 0);

        /* out of memory: what was copied so far goes with dst, when the
         * caller destroys it (fork's error path) */
        if (!d_entry)
            return -ENOMEM;

        memcpy(d_entry, s_entry, sizeof(struct vm_entry));

        if (!(d_entry->qnode = enqueue(&dst->vm_entries, d_entry))) {
            kfree(d_entry);
            return -ENOMEM;
        }

        if (s_entry->vm_anon) {
            s_entry->vm_anon->flags |= VM_COPY;
            vm_anon_incref(s_entry->vm_anon);
        }

        /* the child's entry maps the same object, in its own space */
        d_entry->vm_space = dst;

        if (s_entry->vm_object)
            vm_object_map(s_entry->vm_object, d_entry);

        if (s_entry->flags & (VM_UW|VM_KW) && !(s_entry->flags & VM_SHARED)) {
            /* make the parent's pages read-only: the first write copies */
            vaddr_t sva = s_entry->base;
            vaddr_t eva = sva + s_entry->size;
            unsigned flags = s_entry->flags & ~(VM_UW|VM_KW);
            pmap_protect(src->pmap, sva, eva, flags);
        }
    }

    return 0;
}
