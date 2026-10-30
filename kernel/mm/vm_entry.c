/** \file
 * Map entries: the mappings of an address space. A `struct vm_entry` is
 * one mapping, a range of addresses with its permissions, and what its
 * pages come from (an object, such as a file, and an anon for the pages
 * the process wrote to).
 */

#include <core/system.h>
#include <mm/vm.h>

MALLOC_DEFINE(M_VM_ENTRY, "vm-entry", "virtual memory map entry");

/**
 * Makes a new, empty map entry.
 *
 * @return the entry, zeroed, or NULL if there was no memory for it
 */
struct vm_entry *vm_entry_new(void)
{
    struct vm_entry *vm_entry = kmalloc(sizeof(struct vm_entry), &M_VM_ENTRY, M_ZERO);
    if (!vm_entry) return NULL;

    return vm_entry;
}

/**
 * Releases what a map entry holds: its references to its anon and its
 * object. The entry's structure itself is freed by its caller.
 *
 * @param vm_entry  the entry (NULL does nothing)
 */
void vm_entry_destroy(struct vm_entry *vm_entry)
{
    if (!vm_entry)
        return;

    if (vm_entry->vm_anon)
        vm_anon_decref(vm_entry->vm_anon);

    if (vm_entry->vm_object)
        vm_object_unmap(vm_entry->vm_object, vm_entry);
}
