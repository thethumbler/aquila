/** \file
 * x86: the PC's two 8259 programmable interrupt controllers (PICs).
 *
 * The 8259s bring the interrupts when there's no APIC (or with
 * `apic=off`), as an `x86_irq_chip` (irq.c). With the APIC, they are still
 * set up (their IRQs remapped to vectors 32 to 47, past the exceptions),
 * then masked, and they stay masked.
 */

#include <core/system.h>
#include <core/panic.h>

#include <core/arch.h>
#include <platform/misc.h>
#include <sys/sched.h>
#include <sys/signal.h>
#include <cpu/cpu.h>

#define PIC_CMD   0x00
#define PIC_DATA  0x01

static struct ioaddr master;
static struct ioaddr slave;

/*
 * The 8259s are set up with four initialization command words (ICWs):
 *
 * - ICW1, on each one's command port, starts the setup: edge-triggered,
 *   cascaded (two 8259s), with an ICW4 to come;
 * - ICW2, on the data port, is the vector of its first IRQ: 32 for the
 *   master, 40 for the slave, past the CPU's exceptions;
 * - ICW3 says how they are cascaded: the master has the slave on its IRQ
 *   2, and the slave's ID is 2;
 * - ICW4 sets 8086 mode.
 */

#define ICW1        0x11 /* the same for the master and the slave */
#define ICW2_MASTER 0x20 /* the master's IRQs are vectors 32 and up */
#define ICW2_SLAVE  0x28 /* the slave's are vectors 40 and up */
#define ICW3_MASTER 0x04 /* the master has the slave on its IRQ 2 */
#define ICW3_SLAVE  0x02 /* the slave's ID is 2 */
#define ICW4        0x01 /* 8086 mode */

/* the masks the 8259s have now: the slave's in the high byte */
static uint16_t pic_mask = 0xFFFF;

/** Masks an IRQ at its 8259. */
static void x86_irq_mask(int irq)
{
    if (irq < 8) {  /* the master's */
        pic_mask |= 1 << irq;
        io_out8(&master,  PIC_DATA, pic_mask & 0xFF);
    } else if (irq < 16) {  /* the slave's */
        pic_mask |= 1 << irq;
        io_out8(&slave,  PIC_DATA, (pic_mask >> 8) & 0xFF);
    } else {
        panic("Invalid IRQ number\n");
    }
}

/** Unmasks an IRQ at its 8259 (for the slave's, the master's IRQ 2 too). */
static void x86_irq_unmask(int irq)
{
    if (irq < 8) {  /* the master's */
        pic_mask &= ~(1 << irq);
        io_out8(&master,  PIC_DATA, pic_mask & 0xFF);
    } else if (irq < 16) {  /* the slave's */
        pic_mask &= ~(1 << irq);
        pic_mask &= ~(1 << 2);  /* and the master's IRQ 2, where the slave is */
        io_out8(&slave,  PIC_DATA, (pic_mask >> 8) & 0xFF);
        io_out8(&master, PIC_DATA, pic_mask & 0xFF);
    } else {
        panic("Invalid IRQ number\n");
    }
}

/** Initializes the 8259s, with their IRQs at vectors 32 to 47, so that
 * they don't collide with the CPU's exceptions. */
static void x86_irq_remap(void)
{
    io_out8(&master, PIC_CMD,  ICW1);
    io_out8(&slave,  PIC_CMD,  ICW1);
    io_out8(&master, PIC_DATA, ICW2_MASTER);
    io_out8(&slave,  PIC_DATA, ICW2_SLAVE);
    io_out8(&master, PIC_DATA, ICW3_MASTER);
    io_out8(&slave,  PIC_DATA, ICW3_SLAVE);
    io_out8(&master, PIC_DATA, ICW4);
    io_out8(&slave,  PIC_DATA, ICW4);
}

#define IRQ_ACK 0x20

/** Tells the 8259s that an interrupt is handled: the slave too for its
 * own IRQs. */
static void pic_chip_eoi(unsigned irq)
{
    if (irq >= 16)
        return;

    if (irq > 7) /* the slave's IRQ */
        io_out8(&slave, PIC_CMD, IRQ_ACK);

    io_out8(&master, PIC_CMD, IRQ_ACK);
}

/** Masks an IRQ, for irq.c. */
static void pic_chip_mask(unsigned irq)
{
    if (irq < 16)
        x86_irq_mask(irq);
}

/** Unmasks an IRQ, for irq.c. */
static void pic_chip_unmask(unsigned irq)
{
    if (irq < 16)
        x86_irq_unmask(irq);
}

/** Describes an IRQ's route, for /proc/interrupts. */
static const char *pic_chip_describe(unsigned irq)
{
    return irq < 16 ? "8259 edge" : "";
}

struct x86_irq_chip x86_pic_chip = {
    .name     = "8259",
    .mask     = pic_chip_mask,
    .unmask   = pic_chip_unmask,
    .eoi      = pic_chip_eoi,
    .describe = pic_chip_describe,
};

/** Checks that there is an 8259 at the master's ports: it must keep the
 * mask that was written. Returns 0, or `-ENODEV`. */
static int x86_pic_probe(void)
{
    /* mask all of the slave's IRQs */
    io_out8(&slave, PIC_DATA, 0xFF);

    /* mask all of the master's IRQs, but IRQ 2, where the slave is */
    io_out8(&master, PIC_DATA, 0xFB);

    /* a device answers if it kept the mask */
    if (io_in8(&master, PIC_DATA) != 0xFB)
        return -ENODEV;

    return 0;
}

/** Masks every IRQ of the 8259s, when the APIC brings the interrupts. */
void x86_pic_disable(void)
{
    io_out8(&slave,  PIC_DATA, 0xFF);
    io_out8(&master, PIC_DATA, 0xFF);
}

/**
 * Sets up the 8259s, with every IRQ masked.
 *
 * @param _master  the master's ports (0x20)
 * @param _slave   the slave's ports (0xA0)
 * @return 0, or `-ENODEV` if there's no 8259
 */
int x86_pic_setup(struct ioaddr *_master, struct ioaddr *_slave)
{
    master = *_master;
    slave  = *_slave;

    if (x86_pic_probe()) {
        printk("i8259: Controller not found\n");
        return -ENODEV;
    }

    printk("i8259: initializing [Master: %p (%s), Slave: %p (%s)]\n",
            master.addr, ioaddr_type_str(&master),
            slave.addr, ioaddr_type_str(&slave));

    x86_irq_remap();

    /* every IRQ masked, until it has a handler */
    x86_pic_disable();
    return 0;
}
