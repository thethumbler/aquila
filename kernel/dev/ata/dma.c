/** \file
 * ATA bus master DMA, for IDE controllers that can (such as QEMU's PIIX).
 *
 * With DMA, the controller moves a command's data to or from memory
 * itself, and the request sleeps until the drive's interrupt says it is
 * done. With PIO (`pio.c`), the CPU moves the data a word at a time
 * through an I/O port and polls, which under a hypervisor means an exit
 * to it every time.
 *
 * Each channel transfers through a buffer of its own, which is physically
 * contiguous and doesn't cross a 64 KiB boundary, as the controller
 * requires: a 64 KiB block of the buddy allocator, aligned to its size.
 * One physical region descriptor (PRD) describes it.
 */

#include <core/system.h>
#include <core/string.h>
#include <cpu/io.h>
#include <mm/mm.h>
#include <mm/buddy.h>
#include <sys/sched.h>
#include <aquila/errno.h>

#include <ata.h>

/* the bus master's registers, from its base */
#define BM_CMD          0
#define BM_STATUS       2
#define BM_PRDT         4

#define BM_CMD_START    0x01
#define BM_CMD_READ     0x08    /* from the drive to memory */

#define BM_ST_ACTIVE    0x01
#define BM_ST_ERROR     0x02
#define BM_ST_IRQ       0x04

#define DMA_BUF_SIZE    (64 * 1024)
#define DMA_MAX_SECTORS (DMA_BUF_SIZE / 512)

/** A physical region descriptor: one piece of memory to transfer. */
struct prd {
    uint32_t addr;
    uint16_t size;      /* 0 is 64 KiB */
    uint16_t flags;
} __packed;

#define PRD_EOT         0x8000

/* each channel's descriptor, in the kernel image (its physical address is
 * LMA()), and its buffer, from the buddy allocator (dma_init()) */
static struct prd prdt[2] __aligned(16);
static paddr_t dma_buf_paddr[2];
static char *dma_buf[2];

/** Returns which channel a drive is on: 0 (primary) or 1 (secondary). */
static int channel_index(struct ata_drive *drive)
{
    return drive->id / 2;
}

/** Ends a transfer: keeps what the controller and the drive say about it,
 * which also acknowledges the interrupt, and marks it done. */
static void dma_complete(struct ata_channel *ch, uint8_t bm_status)
{
    ch->status = io_in8(&ch->base, ATA_REG_STATUS);
    ch->bm_status = bm_status;
    io_out8(&ch->bm, BM_STATUS, BM_ST_IRQ | BM_ST_ERROR);
    ch->done = 1;
}

/**
 * Handles a channel's interrupt. If it ends a DMA transfer, the request
 * waiting for it wakes up; otherwise (PIO polls) reading the drive's
 * status acknowledges it.
 *
 * @param ch  the channel
 */
void dma_irq(struct ata_channel *ch)
{
    uint8_t bm_status = ch->bm.addr ? io_in8(&ch->bm, BM_STATUS) : 0;

    /* not from a DMA transfer: reading the status acknowledges it */
    if (!(bm_status & BM_ST_IRQ)) {
        io_in8(&ch->base, ATA_REG_STATUS);
        return;
    }

    dma_complete(ch, bm_status);
    waitq_wakeup_nopoll(&ch->irq_wait);
}

/** Transfers up to `DMA_MAX_SECTORS` sectors between the drive and the
 * channel's buffer, and waits until it is done. Returns 0, or `-EIO` if
 * the controller or the drive reports an error. */
static int dma_transfer(struct ata_drive *drive, uint64_t lba, size_t count, int write)
{
    struct ata_channel *ch = drive->channel;
    int c = channel_index(drive);

    prdt[c].addr  = (uint32_t) dma_buf_paddr[c];
    prdt[c].size  = (count * 512) & 0xFFFF;
    prdt[c].flags = PRD_EOT;

    /* stop the bus master, set the direction and the descriptor table,
     * and clear its interrupt and error bits */
    io_out8(&ch->bm, BM_CMD, write ? 0 : BM_CMD_READ);
    io_out32(&ch->bm, BM_PRDT, (uint32_t) (uintptr_t) LMA(&prdt[c]));
    io_out8(&ch->bm, BM_STATUS, BM_ST_IRQ | BM_ST_ERROR);

    ata_setup(drive, lba, count);
    ch->done = 0;

    if (drive->mode == ATA_MODE_LBA48)
        io_out8(&drive->base, ATA_REG_CMD, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT);
    else
        io_out8(&drive->base, ATA_REG_CMD, write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);

    io_out8(&ch->bm, BM_CMD, (write ? 0 : BM_CMD_READ) | BM_CMD_START);

    /* a process sleeps until the interrupt; before there are processes
     * (mounting the root, loading init at boot), the status is polled */
    while (!ch->done) {
        if (sched_running && curthread) {
            waitq_sleep_unint(&ch->irq_wait);
        } else {
            uint8_t bm_status = io_in8(&ch->bm, BM_STATUS);

            if (bm_status & BM_ST_IRQ)
                dma_complete(ch, bm_status);
        }
    }

    io_out8(&ch->bm, BM_CMD, 0);

    if ((ch->bm_status & BM_ST_ERROR) || (ch->status & (ATA_STATUS_ERR | ATA_STATUS_DF))) {
        printk("ata%d: DMA %s error, status 0x%x, bus master 0x%x\n", drive->id,
                write ? "write" : "read", ch->status, ch->bm_status);
        return -EIO;
    }

    return 0;
}

/** Reads `count` sectors from `lba` on, through the channel's buffer.
 * Returns 0 or a negative errno. */
static ssize_t dma_read(struct ata_drive *drive, uint64_t lba, size_t count, void *buf)
{
    char *cbuf = buf;

    while (count) {
        size_t n = MIN(count, DMA_MAX_SECTORS);
        int err;

        if ((err = dma_transfer(drive, lba, n, 0)))
            return err;

        memcpy(cbuf, dma_buf[channel_index(drive)], n * 512);

        lba   += n;
        count -= n;
        cbuf  += n * 512;
    }

    return 0;
}

/** Writes `count` sectors from `lba` on, through the channel's buffer.
 * Returns 0 or a negative errno. */
static ssize_t dma_write(struct ata_drive *drive, uint64_t lba, size_t count, void *buf)
{
    char *cbuf = buf;

    while (count) {
        size_t n = MIN(count, DMA_MAX_SECTORS);
        int err;

        memcpy(dma_buf[channel_index(drive)], cbuf, n * 512);

        if ((err = dma_transfer(drive, lba, n, 1)))
            return err;

        lba   += n;
        count -= n;
        cbuf  += n * 512;
    }

    return 0;
}

/**
 * Sets a drive up for DMA, if both the drive and its channel's controller
 * can do it.
 *
 * @param drive  the drive
 * @return 0 if the drive uses DMA now, `-ENODEV` if it can't, or `-ENOMEM`
 *         if there is no memory for its channel's buffer
 */
int dma_init(struct ata_drive *drive)
{
    if (!drive->channel->bm.addr || !(drive->capabilities & ATA_CAP_DMA))
        return -ENODEV;

    /* the channel's buffer, once for its two drives: a block of the buddy
     * allocator is aligned to its size, so it crosses no 64 KiB boundary,
     * and below 16 MiB its address fits the descriptor's 32 bits */
    int c = channel_index(drive);

    if (!dma_buf[c]) {
        paddr_t paddr = buddy_alloc(BUDDY_ZONE_DMA, DMA_BUF_SIZE);

        if (paddr == (paddr_t) -1)
            return -ENOMEM;

        dma_buf_paddr[c] = paddr;
        dma_buf[c] = PHYS2VIRT(paddr);
    }

    drive->read  = dma_read;
    drive->write = dma_write;
    return 0;
}
