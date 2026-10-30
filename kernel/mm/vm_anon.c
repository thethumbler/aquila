/** \file
 * Anons: the pages a process has written to, in one of its mappings.
 *
 * An anon (`struct vm_anon`) holds a mapping's private pages, each through
 * an aref (`struct vm_aref`), keyed by its offset in the mapping. A fork
 * shares the anon, and its arefs, between parent and child, copy on write:
 * the first one to write a shared page gets its own copy (`mm/fault.c`).
 * This is a simplified form of NetBSD's UVM amaps and anons.
 */

#include <core/system.h>
#include <core/panic.h>
#include <mm/vm.h>

MALLOC_DEFINE(M_VM_ANON, "vm-anon", "anonymous virtual memory object");

/** Says whether an aref is the one for an offset (`_b`, a `size_t *`), for
 * the anon's hashmap. An aref without a page is for no offset. */
static int vm_aref_eq(void *_a, void *_b)
{
    struct vm_aref *a = (struct vm_aref *) _a;

    if (!a->vm_page)
        return 0;

    size_t *b = (size_t *) _b;

    return a->vm_page->off == *b;
}

/**
 * Makes a new, empty anon.
 *
 * @return the anon, with no references, or NULL if there was no memory
 */
struct vm_anon *vm_anon_new(void)
{
    struct vm_anon *vm_anon = kmalloc(sizeof(struct vm_anon), &M_VM_ANON, M_ZERO);
    if (!vm_anon) goto error;

    vm_anon->arefs = hashmap_new(0, vm_aref_eq);

    if (!vm_anon->arefs)
        goto error;

    return vm_anon;

error:
    if (vm_anon) {
        if (vm_anon->arefs)
            hashmap_free(vm_anon->arefs);
        kfree(vm_anon);
    }

    return NULL;
}

/**
 * Releases what an aref holds. An aref holds nothing besides its page,
 * which its anon frees, so this does nothing.
 *
 * @param vm_aref  the aref
 */
void vm_aref_destroy(struct vm_aref *vm_aref)
{
    /* nothing to do */
}

/**
 * Drops a reference to an aref.
 *
 * @param vm_aref  the aref
 */
void vm_aref_decref(struct vm_aref *vm_aref)
{
    vm_aref->ref--;
}

/**
 * Releases an anon's arefs, and the pages of those that no other anon
 * shares. The anon's structure itself is freed by its caller.
 *
 * @param vm_anon  the anon (NULL does nothing)
 */
void vm_anon_destroy(struct vm_anon *vm_anon)
{
    if (!vm_anon) return;

    struct hashmap *arefs = vm_anon->arefs;

    if (!arefs)
        return;

    hashmap_for (qnode, vm_anon->arefs) {
        struct hashmap_node *hnode = (struct hashmap_node *) qnode->value;
        struct vm_aref *aref = (struct vm_aref *) hnode->entry;

        vm_aref_decref(aref);

        if (!aref->ref) {
            if (aref->vm_page) {
                mm_page_dealloc(aref->vm_page->paddr);
            }

            kfree(aref);
        }
    }

    hashmap_free(vm_anon->arefs);
}

/**
 * Adds a reference to an anon: another mapping shares it (a fork).
 *
 * @param vm_anon  the anon (NULL does nothing)
 */
void vm_anon_incref(struct vm_anon *vm_anon)
{
    if (!vm_anon)
        return;

    vm_anon->ref++;
}

/**
 * Drops a reference to an anon, and frees it with the last one.
 *
 * @param vm_anon  the anon (NULL does nothing)
 */
void vm_anon_decref(struct vm_anon *vm_anon)
{
    if (!vm_anon)
        return;

    vm_anon->ref--;

    if (!vm_anon->ref) {
        vm_anon_destroy(vm_anon);
        kfree(vm_anon);
    }
}

/** Puts all of `src`'s arefs in `dst` as well, shared: each gets one more
 * reference. Returns 0, `-EINVAL`, or `-ENOMEM` if there was no memory
 * for one; the arefs put in `dst` before that have their reference, and
 * go with `vm_anon_destroy(dst)`. */
static int vm_anon_copy_arefs(struct vm_anon *src, struct vm_anon *dst)
{
    if (!src || !dst || !src->arefs || !dst->arefs)
        return -EINVAL;

    hashmap_for (qnode, src->arefs) {
        struct hashmap_node *node = (struct hashmap_node *) qnode->value;
        struct vm_aref *aref = (struct vm_aref *) node->entry;

        if (hashmap_insert(dst->arefs, node->hash, aref))
            return -ENOMEM;

        aref->ref++;
    }

    return 0;
}

/**
 * Makes a copy of an anon that shares its arefs, and so its pages, when
 * a mapping's shared anon is written to after a fork. The pages
 * themselves are copied later, one by one, as they are written to.
 *
 * @param vm_anon  the anon to copy
 * @return the new anon, with one reference, or NULL if there was no
 *         memory for it
 */
struct vm_anon *vm_anon_copy(struct vm_anon *vm_anon)
{
    if (!vm_anon)
        return NULL;

    struct vm_anon *new_anon = vm_anon_new();

    if (!new_anon)
        return NULL;

    new_anon->flags = vm_anon->flags & ~VM_COPY;
    new_anon->ref   = 1;

    /* on failure, the arefs copied so far give back their references,
     * and the hashmap goes */
    if (vm_anon_copy_arefs(vm_anon, new_anon)) {
        vm_anon_destroy(new_anon);
        kfree(new_anon);
        return NULL;
    }

    return new_anon;
}

/**
 * Splits an anon in two, for a mapping split in two (`vm_space_split()`):
 * the arefs of pages at offset `off` or past it move to a new anon, and
 * the others stay. The arefs move, so the pages stay shared with the
 * anons a fork shared them with, copy on write.
 *
 * @param vm_anon  the anon, which only one mapping may have
 * @param off      the offset where the new anon starts
 * @return the new anon, with one reference, or NULL if there was no
 *         memory for it; `vm_anon` is as it was then
 */
struct vm_anon *vm_anon_split(struct vm_anon *vm_anon, size_t off)
{
    struct vm_anon *new_anon = vm_anon_new();

    if (!new_anon)
        return NULL;

    new_anon->flags = vm_anon->flags & ~VM_COPY;
    new_anon->ref   = 1;

    struct hashmap *arefs = vm_anon->arefs;

    /* the arefs go in the new anon first: if there's no memory for
     * them, the old anon hasn't changed */
    hashmap_for (qnode, arefs) {
        struct hashmap_node *node = (struct hashmap_node *) qnode->value;
        struct vm_aref *aref = (struct vm_aref *) node->entry;

        if (aref->vm_page->off >= off && hashmap_insert(new_anon->arefs, node->hash, aref)) {
            hashmap_free(new_anon->arefs);
            kfree(new_anon);
            return NULL;
        }
    }

    /* then they come off the old one, which frees their nodes there */
    for (size_t i = 0; i < arefs->buckets_nr; ++i) {
        struct qnode *qnode = arefs->buckets[i].head;

        while (qnode) {
            struct qnode *next = qnode->next;
            struct hashmap_node *node = (struct hashmap_node *) qnode->value;

            if (((struct vm_aref *) node->entry)->vm_page->off >= off)
                hashmap_node_remove(arefs, node);

            qnode = next;
        }
    }

    return new_anon;
}
