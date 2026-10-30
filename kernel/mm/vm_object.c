/** \file
 * Vm objects: what a mapping's pages come from, such as a file. A file's
 * object is its vnode's, and keeps the file's pages that have been read in
 * (the page cache).
 */

#include <core/system.h>
#include <core/arch.h>
#include <core/panic.h>
#include <mm/mm.h>
#include <mm/pmap.h>
#include <mm/vm.h>
#include <ds/queue.h>
#include <ds/hashmap.h>
#include <fs/vfs.h>

MALLOC_DEFINE(M_VM_OBJECT, "vm-object", "virtual memory object");

/**
 * Adds a reference to a vm object: one more mapping of it. A file's object
 * holds its vnode while it's mapped, because the pages are read from it,
 * and the program file it runs may be removed meanwhile.
 *
 * @param vm_object  the object
 */
void vm_object_incref(struct vm_object *vm_object)
{
    if (!vm_object->ref++ && vm_object->type == VMOBJ_FILE)
        ((struct vnode *) vm_object->p)->ref++;
}

/**
 * Drops a reference to a vm object: one mapping of it less. A file's
 * object lets go of its vnode with the last one. Its pages stay, cached
 * with the vnode, until the vnode is freed (`vnode_pages_free()`).
 *
 * @param vm_object  the object
 */
void vm_object_decref(struct vm_object *vm_object)
{
    if (!--vm_object->ref && vm_object->type == VMOBJ_FILE)
        vfs_close((struct vnode *) vm_object->p);
}

/**
 * Makes a map entry a mapping of a vm object: the entry holds a
 * reference to it (`vm_object_incref()`), and goes on the object's list
 * of mappings, through which its pages are found in page tables
 * (`vm_object_page_unmap()`). The entry's `vm_space` must be set.
 *
 * @param vm_object  the object
 * @param vm_entry   the entry, which isn't on a list of mappings
 */
void vm_object_map(struct vm_object *vm_object, struct vm_entry *vm_entry)
{
    vm_entry->vm_object = vm_object;
    vm_entry->obj_link = (struct list_node) {0};
    list_append(&vm_object->mappings, &vm_entry->obj_link);
    vm_object_incref(vm_object);
}

/**
 * Undoes `vm_object_map()`: the entry leaves the object's list of
 * mappings, and drops its reference.
 *
 * @param vm_object  the object
 * @param vm_entry   the entry
 */
void vm_object_unmap(struct vm_object *vm_object, struct vm_entry *vm_entry)
{
    list_remove(&vm_entry->obj_link);
    vm_object_decref(vm_object);
}

/**
 * Takes a page of a vm object out of the page tables that map it, so that
 * it can be freed: each mapping of the object that covers the page's
 * offset has its page table entry there removed, if it is this page (a
 * private mapping may have its own copy there, which stays). A process
 * that touches the address again faults, and gets the page read in again.
 *
 * @param vm_object  the object
 * @param vm_page    its page
 */
void vm_object_page_unmap(struct vm_object *vm_object, struct vm_page *vm_page)
{
    list_for (node, &vm_object->mappings) {
        struct vm_entry *vm_entry = list_entry(node, struct vm_entry, obj_link);

        if (vm_page->off < vm_entry->off || vm_page->off - vm_entry->off >= vm_entry->size)
            continue;

        struct pmap *pmap = vm_entry->vm_space->pmap;
        vaddr_t vaddr = vm_entry->base + (vm_page->off - vm_entry->off);

        if (PAGE_ALIGN(arch_page_get_mapping(pmap, vaddr)) == vm_page->paddr)
            pmap_remove(pmap, vaddr, vaddr + PAGE_SIZE);
    }
}

/**
 * Makes a page of a vm object read only in the page tables that map it,
 * once it is written back to its file: the next write to it, through any
 * mapping, faults, and makes it dirty again (`vm_cache_page_dirty()`).
 *
 * @param vm_object  the object
 * @param vm_page    its page
 */
void vm_object_page_protect(struct vm_object *vm_object, struct vm_page *vm_page)
{
    list_for (node, &vm_object->mappings) {
        struct vm_entry *vm_entry = list_entry(node, struct vm_entry, obj_link);

        if (vm_page->off < vm_entry->off || vm_page->off - vm_entry->off >= vm_entry->size)
            continue;

        struct pmap *pmap = vm_entry->vm_space->pmap;
        vaddr_t vaddr = vm_entry->base + (vm_page->off - vm_entry->off);

        if (PAGE_ALIGN(arch_page_get_mapping(pmap, vaddr)) == vm_page->paddr)
            pmap_protect(pmap, vaddr, vaddr + PAGE_SIZE, (vm_entry->flags & VM_PERM) & ~(VM_UW | VM_KW));
    }
}

/**
 * Adds a page to a vm object, at the page's offset (`vm_page->off`).
 *
 * @param vm_object  the object
 * @param vm_page    the page
 * @return 0, or `-ENOMEM` if there was no memory to add it (the object
 *         doesn't have the page then)
 */
int vm_object_page_insert(struct vm_object *vm_object, struct vm_page *vm_page)
{
    size_t off = vm_page->off;
    hash_t hash = hashmap_digest(&off, sizeof(off));
    return hashmap_insert(vm_object->pages, hash, vm_page) ? -ENOMEM : 0;
}
