/** \file
 * Restarting, halting and powering off the machine (`reboot()`), and
 * Ctrl+Alt+Del.
 *
 * Ctrl+Alt+Del restarts the machine right away, unless init asked for
 * `SIGINT` instead (`RB_DISABLE_CAD`), to shut down cleanly.
 */

#include <core/system.h>
#include <core/arch.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/reboot.h>
#include <dev/dev.h>

#include <aquila/errno.h>

static int cad_restarts = 1;

/**
 * Carries out a `reboot()` command. Before restarting, halting or powering
 * off, the disks' caches are written out.
 *
 * @param cmd  `RB_AUTOBOOT`, `RB_HALT_SYSTEM`, `RB_POWER_OFF`,
 *             `RB_ENABLE_CAD` or `RB_DISABLE_CAD`
 * @return 0 for `RB_ENABLE_CAD` and `RB_DISABLE_CAD`, or `-EINVAL` for
 *         an unknown command; the others don't return
 */
int kernel_reboot(unsigned cmd)
{
    /* nothing may be left in the page cache or the disks' write caches */
    if (cmd == RB_AUTOBOOT || cmd == RB_HALT_SYSTEM || cmd == RB_POWER_OFF) {
        vm_cache_flush_all();
        kdev_sync();
    }

    switch (cmd) {
        case RB_AUTOBOOT:
            printk("kernel: restarting system\n");
            arch_reboot();
            printk("kernel: restart failed, halting\n");
            arch_halt();
        case RB_HALT_SYSTEM:
            printk("kernel: system halted\n");
            arch_halt();
        case RB_POWER_OFF:
            printk("kernel: power down\n");
            arch_poweroff();
        case RB_ENABLE_CAD:
            cad_restarts = 1;
            return 0;
        case RB_DISABLE_CAD:
            cad_restarts = 0;
            return 0;
    }

    return -EINVAL;
}

/**
 * Handles Ctrl+Alt+Del from the keyboard, possibly in an interrupt: it
 * restarts the machine, or sends `SIGINT` to init if init asked for that.
 */
void kernel_ctrlaltdel(void)
{
    /* restart right away, as a reset would: writing out the caches would
     * wait for the disk, which an interrupt can't do */
    if (cad_restarts) {
        printk("kernel: restarting system\n");
        arch_reboot();
        return;
    }

    struct proc *init = proc_pid_find(1);

    if (init)
        signal_proc_send(init, SIGINT);
}
