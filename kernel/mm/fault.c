/** \file
 * Page faults: a process's page brought in when it first touches it.
 *
 * A process's memory is mapped lazily. A page is mapped only when the
 * process first reads or writes it, and the fault that the access raises
 * comes here. The page comes from one of three places, tried in order:
 *
 * 1. the process's anon (`vm_anon`), the pages it has written to, which
 *    copy on write when a fork shares them;
 * 2. the object the mapping is of (`vm_object`): a file's pages, read in
 *    by its pager;
 * 3. a new page, zeroed.
 *
 * See docs/kernel/memory.md.
 */

#include <core/system.h>
#include <core/panic.h>
#include <core/arch.h>

#include <mm/pmap.h>
#include <mm/vm.h>

#include <sys/sched.h>
#include <sys/signal.h>

/** What a page fault is about, passed to each of the steps below. */
struct pf {
    int flags;
    vaddr_t addr;

    struct vm_space *vm_space;
    struct vm_entry *vm_entry;

    size_t off;     /* the page's offset in the mapping's object or anon */
    hash_t hash;    /* `off` hashed, to look it up */
};

/** Returns whether an access of kind `flags` (`PF_*`) is one the mapping's
 * permissions `vm_flags` don't allow. */
static inline int check_violation(int flags, int vm_flags)
{
    return
    ((flags & PF_READ)  && !(vm_flags & VM_UR)) ||
    ((flags & PF_WRITE) && !(vm_flags & VM_UW)) ||
    ((flags & PF_EXEC)  && !(vm_flags & VM_UX));
}

/**
 * Handles a fault on a page that is mapped, but not with the permission
 * the access needed: a write to a page that is read-only because a fork
 * shared it. If only this process has the page, it gets it read-write,
 * after copying it if the page is still shared with an object.
 *
 * Returns 1 if the fault is handled, 0 if another step has to handle it,
 * or a negative errno.
 */
static inline int pf_present(struct pf *pf)
{
    /* the page is mapped, but without the permission the access needed */
    struct vm_entry *vm_entry = pf->vm_entry;
    struct vm_anon  *vm_anon  = vm_entry->vm_anon;

    struct pmap *pmap = pf->vm_space->pmap;

    /* with no anon, or a shared one, this step can't handle it: the
     * next steps do */
    if (!vm_anon || vm_anon->ref != 1)
        return 0;

    /* only this process has the anon */
    struct hashmap_node *hash_node = hashmap_lookup(vm_anon->arefs, pf->hash, &pf->off);
    struct vm_aref *vm_aref = (struct vm_aref *) (hash_node? hash_node->entry : NULL);

    if (!vm_aref || vm_aref->ref != 1)
        return 0;

    if (vm_aref->flags & VM_COPY) {
        /* copy the page */
        struct vm_page *new_page = mm_page_alloc();

        if (!new_page)
            return -ENOMEM;

        new_page->off = vm_aref->vm_page->off;
        new_page->ref = 1;
        new_page->vm_object = NULL;

        pmap_page_copy(vm_aref->vm_page->paddr, new_page->paddr);

        mm_page_decref(vm_aref->vm_page->paddr);

        vm_aref->vm_page = new_page;
        vm_aref->flags &= ~VM_COPY;

        /* if there's no memory for a page table, the copy is the aref's
         * already, and the next fault maps it */
        int err = mm_page_map(pmap, pf->addr, new_page->paddr, pf->vm_entry->flags & VM_PERM);

        if (err)
            return err;
    } else {
        /* only this process has the page: just change its permissions */
        pmap_protect(pmap, pf->addr, pf->addr+PAGE_SIZE, pf->vm_entry->flags & VM_PERM);
    }

    return 1;
}

/**
 * Handles a fault on a page that the process's anon has (a page it wrote
 * to before). A shared anon is copied first, if the fork that shared it
 * asked for that. A read maps the page read-only; a write maps it
 * read-write if only this process has it, and copies it otherwise.
 *
 * Returns 1 if the fault is handled, 0 if the anon doesn't have the page,
 * or a negative errno.
 */
static inline int pf_anon(struct pf *pf)
{
    struct vm_entry *vm_entry = pf->vm_entry;
    struct vm_anon  *vm_anon  = vm_entry->vm_anon;

    struct pmap *pmap = pf->vm_space->pmap;

    if (vm_anon->flags & VM_COPY) {

        if (vm_anon->ref > 1) {
            struct vm_anon *new_anon = vm_anon_copy(vm_anon);

            if (!new_anon)
                return -ENOMEM;

            vm_anon_decref(vm_anon);
            vm_entry->vm_anon = new_anon;
            vm_anon = new_anon;
        }

        vm_anon->flags &= ~VM_COPY;
    }

    struct hashmap_node *aref_node = hashmap_lookup(vm_anon->arefs, pf->hash, &pf->off);

    if (!aref_node || !aref_node->entry)
        return 0;

    struct vm_aref *aref = (struct vm_aref *) aref_node->entry;

    if (!aref->vm_page)
        panic("aref has no page");

    if (!(pf->flags & PF_WRITE)) {
        /* a read: map it read-only */
        struct vm_page *vm_page = aref->vm_page;

        uint32_t perm = (vm_entry->flags & VM_PERM) & ~(VM_UW|VM_KW);
        int err = mm_page_map(pmap, pf->addr, vm_page->paddr, perm);

        if (err)
            return err;

        mm_page_incref(vm_page->paddr);
        return 1;
    }

    /* a write */

    if (aref->ref == 1) {
        /* only this process has the page: just map it */

        struct vm_page *vm_page = aref->vm_page;

        uint32_t perm = vm_entry->flags & VM_PERM;
        int err = mm_page_map(pmap, pf->addr, vm_page->paddr, perm);

        if (err)
            return err;

        mm_page_incref(vm_page->paddr);
        return 1;
    }

    /* the page is shared: copy it, and map the copy read-write */

    /* get the copy and its aref first, so that if there's no memory for
     * them, nothing has changed */
    struct vm_aref *new_aref = kmalloc(sizeof(struct vm_aref), &M_VM_AREF, M_ZERO);
    struct vm_page *new_page = new_aref ? mm_page_alloc() : NULL;

    if (!new_page) {
        kfree(new_aref);
        return -ENOMEM;
    }

    struct vm_page *vm_page = aref->vm_page;

    new_page->off = vm_page->off;
    new_page->ref = 1;
    new_page->vm_object = NULL;

    pmap_page_copy(vm_page->paddr, new_page->paddr);

    new_aref->ref = 1;
    new_aref->flags = aref->flags;
    new_aref->vm_page = new_page;

    /* the anon's page at this offset is the copy now (the key is the
     * same, so its hashmap node takes the new aref) */
    aref_node->entry = new_aref;
    aref->ref--;

    /* if there's no memory for a page table, the copy is the anon's
     * already, and the next fault maps it */
    int err = mm_page_map(pmap, pf->addr, new_page->paddr, vm_entry->flags & VM_PERM);

    return err ? err : 1;
}

/** Returns an object's page at offset `off`, reading it in with the
 * object's pager if the object doesn't have it yet. Returns NULL if it
 * couldn't be read in (no memory, or a read error). */
static inline struct vm_page *vm_object_page(struct vm_object *vm_object, hash_t hash, size_t off)
{
    struct hashmap_node *hash_node = hashmap_lookup(vm_object->pages, hash, &off);
    struct vm_page *vm_page = NULL;

    if (hash_node) {
        /* the object has the page */
        vm_page = (struct vm_page *) hash_node->entry;
    } else {
        /* it doesn't: page it in */
        if (vm_object->pager && vm_object->pager->in)
            vm_page = vm_object->pager->in(vm_object, off);
        else
            printk("vm: an object with no pager, page %p missing\n", (void *) off);
    }

    return vm_page;
}

/**
 * Handles a fault on a page of the object the mapping is of (a file's
 * page). A read-only mapping maps the object's page itself. A writable
 * one gets a copy, in the process's anon: what it writes is its own.
 *
 * Returns 1 if the fault is handled, or a negative errno (`-EIO` if the
 * page couldn't be read in).
 */
static inline int pf_object(struct pf *pf)
{
    struct vm_entry *vm_entry = pf->vm_entry;

    struct vm_object *vm_object = vm_entry->vm_object;
    struct vm_page *vm_page = NULL;
    struct pmap *pmap = pf->vm_space->pmap;

    /* the object's page, read in if needed */
    vm_page = vm_object_page(vm_object, pf->hash, pf->off);

    if (!vm_page)
        return -EIO;

    if (!(vm_entry->flags & VM_UW)) {
        /* a read-only mapping: map the object's page */
        int err = mm_page_map(pmap, pf->addr, vm_page->paddr, vm_entry->flags & VM_PERM);

        if (err)
            return err;

        mm_page_incref(vm_page->paddr);
        return 1;
    }

    /* a writable mapping: the process gets its own copy, in its anon */

    /* make the anon if there's none yet */
    if (!vm_entry->vm_anon) {
        if (!(vm_entry->vm_anon = vm_anon_new()))
            return -ENOMEM;

        vm_entry->vm_anon->ref = 1;
    }

    struct vm_aref *vm_aref = kmalloc(sizeof(struct vm_aref), &M_VM_AREF, M_ZERO);

    if (!vm_aref)
        return -ENOMEM;

    vm_aref->ref = 1;

    /* copy the page */
    struct vm_page *new_page = mm_page_alloc();

    if (!new_page) {
        kfree(vm_aref);
        return -ENOMEM;
    }

    new_page->off = vm_page->off;
    new_page->ref = 1;
    new_page->vm_object = NULL;

    pmap_page_copy(vm_page->paddr, new_page->paddr);

    vm_aref->vm_page = new_page;

    if (hashmap_insert(vm_entry->vm_anon->arefs, pf->hash, vm_aref)) {
        mm_page_dealloc(new_page->paddr);
        kfree(vm_aref);
        return -ENOMEM;
    }

    /* if there's no memory for a page table, the copy is the anon's
     * already, and the next fault maps it */
    int err = mm_page_map(pmap, pf->addr, new_page->paddr, vm_entry->flags & VM_PERM);

    return err ? err : 1;
}

/**
 * Handles a fault on a page that nothing has yet: a new page, zeroed, in
 * the process's anon.
 *
 * Returns 1 if the fault is handled, or a negative errno.
 */
static inline int pf_zero(struct pf *pf)
{
    struct vm_entry *vm_entry = pf->vm_entry;
    struct pmap *pmap = pf->vm_space->pmap;

    if (!vm_entry->vm_anon) {
        if (!(vm_entry->vm_anon = vm_anon_new()))
            return -ENOMEM;

        vm_entry->vm_anon->ref = 1;
    }

    struct vm_aref *vm_aref = kmalloc(sizeof(struct vm_aref), &M_VM_AREF, M_ZERO);

    if (!vm_aref)
        return -ENOMEM;

    struct vm_page *new_page = mm_page_alloc();

    if (!new_page) {
        kfree(vm_aref);
        return -ENOMEM;
    }

    new_page->off = pf->off;
    new_page->ref = 1;

    vm_aref->vm_page = new_page;
    vm_aref->ref = 1;

    /* zero the page through its mapping (writable by the kernel only, for
     * now) before the anon has it: a page that isn't zeroed yet is never
     * any process's, so another process's old data can't leak */
    int err = mm_page_map(pmap, pf->addr, new_page->paddr, VM_KW);

    if (!err) {
        memset((void *) pf->addr, 0, PAGE_SIZE);
        pmap_protect(pmap, pf->addr, pf->addr + PAGE_SIZE, vm_entry->flags & VM_PERM);
        err = hashmap_insert(vm_entry->vm_anon->arefs, pf->hash, vm_aref);

        if (err)
            pmap_remove(pmap, pf->addr, pf->addr + PAGE_SIZE);
    }

    if (err) {
        mm_page_dealloc(new_page->paddr);
        kfree(vm_aref);
        return err;
    }

    return 1;
}

/**
 * Handles a page fault in the current process's address space. The
 * architecture calls it from its page fault handler.
 *
 * If the address isn't mapped, or the mapping doesn't allow the access,
 * the fault can't be resolved, and the architecture decides what follows:
 * a `SIGSEGV`, a failed copy (`EFAULT`) for a system call, or a panic for
 * the kernel's own access. If there's no memory to resolve it, the
 * process is also killed (`SIGKILL`): it couldn't go on.
 *
 * @param vaddr  the address that faulted
 * @param flags  the kind of access (`PF_READ`, `PF_WRITE`, `PF_EXEC`, and
 *               `PF_PRESENT` if a page was mapped there)
 * @return 0 if the fault is resolved (the page is mapped now), or
 *         `-EACCES` if the mapping doesn't allow the access, or `-EFAULT`
 *         if it can't be resolved otherwise (no mapping there): a
 *         `SIGSEGV`'s `SEGV_ACCERR` and `SEGV_MAPERR`
 */
int mm_page_fault(vaddr_t vaddr, int flags)
{
    vaddr_t addr = PAGE_ALIGN(vaddr);

    struct vm_space *vm_space = &curproc->vm_space;
    struct pmap *pmap = vm_space->pmap;
    struct vm_entry *vm_entry = NULL;

    /* the mapping the address is in */
    vm_entry = vm_space_find(vm_space, addr);

    /* no mapping, or one that doesn't allow the access: SIGSEGV */
    if (!vm_entry)
        goto sigsegv;

    if (check_violation(flags, vm_entry->flags))
        return -EACCES;

    /* the page's offset in the mapping's object (and anon) */
    size_t off = addr - vm_entry->base + vm_entry->off;

    hash_t hash = hashmap_digest(&off, sizeof(off));

    struct pf pf = {
        .flags = flags,
        .addr = addr,
        .vm_space = vm_space,
        .vm_entry = vm_entry,
        .off = off,
        .hash = hash,
    };

    /* each step returns 1 if the page is mapped now, 0 if the fault isn't
     * that step's to handle, or a negative errno if it can't be resolved
     * (no memory, or the file's page couldn't be read), which is then
     * like an address that isn't the process's */
    int ret = 0;

    /* mapped, without the permission: a write to a shared page */
    if (flags & PF_PRESENT && (ret = pf_present(&pf)))
        goto done;

    /* a page the process's anon has */
    if (vm_entry->vm_anon && (ret = pf_anon(&pf)))
        goto done;

    /* a page of the object the mapping is of */
    if (vm_entry->vm_object && (ret = pf_object(&pf)))
        goto done;

    /* a new page, zeroed */
    ret = pf_zero(&pf);

done:
    if (ret > 0)
        return 0;

    if (ret < 0)
        printk("[%d:%d] %s: page fault at %p not resolved: error %d\n",
               curproc->pid, curthread->tid, curproc->name, (void *) vaddr, -ret);

    /* out of memory: the process can't go on (the access would only fault
     * again), so it's killed; if the fault came from a system call's copy
     * from or to its memory, the copy fails (EFAULT), and the process is
     * killed on its way back to user mode */
    if (ret == -ENOMEM)
        signal_proc_send(curproc, SIGKILL);

sigsegv:
    return -EFAULT;
}
