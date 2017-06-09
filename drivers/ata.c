#include <levos/kernel.h>
#include <levos/ata.h>
#include <levos/page.h>
#include <levos/arch.h>
#include <levos/device.h>
#include <levos/intr.h>

#define ATA_PRIMARY_IRQ 14
#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_DCR_AS 0x3F6

#define ATA_SECONDARY_IRQ 15
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_DCR_AS 0x376

#define ATA_PRIMARY 0x00
#define ATA_SECONDARY 0x01

#define ATA_MASTER 0x00
#define ATA_SLAVE  0x01

#define MODULE_NAME ata

static struct device *ata_devices[4];
static int n_ata_devices;

struct dma_prdt {
    uint32_t prdt_offset;
    uint16_t prdt_bytes;
    uint16_t prdt_last;
} __packed;

struct ata_dma_priv {
    int adp_busmaster;
    int adp_last_count;
    char *adp_dma_area;
    struct dma_prdt *adp_dma_prdt;
};

void ide_select_drive(uint8_t bus, uint8_t i)
{
    if (bus == ATA_PRIMARY)
        if (i == ATA_MASTER)
            outportb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xA0);
        else outportb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xB0);
    else
        if (i == ATA_MASTER)
            outportb(ATA_SECONDARY_IO + ATA_REG_HDDEVSEL, 0xA0);
        else outportb(ATA_SECONDARY_IO + ATA_REG_HDDEVSEL, 0xB0);
}

void ide_400ns_delay(uint16_t io)
{
    for (int i = 0; i < 4; i++)
        inportb(io + ATA_REG_ALTSTATUS);
}

void
ide_poll(uint16_t io)
{
    uint8_t status;

    /* read the ALTSTATUS 4 times */
    inportb(io + ATA_REG_ALTSTATUS);
    inportb(io + ATA_REG_ALTSTATUS);
    inportb(io + ATA_REG_ALTSTATUS);
    inportb(io + ATA_REG_ALTSTATUS);

    /* now wait for the BSY bit to clear */
    while ((status = inportb(io + ATA_REG_STATUS)) & ATA_SR_BSY)
        ;

    status = inportb(io + ATA_REG_STATUS);

    if (status & ATA_SR_ERR)
        panic("ATA ERR bit is set\n");

    return;
}

int
ata_status_wait(int io_base, int timeout) {
	int status;

	if (timeout > 0) {
		int i = 0;
		while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY && (i < timeout)) i++;
	} else {
		while ((status = inportb(io_base + ATA_REG_STATUS)) & ATA_SR_BSY);
	}
	return status;
}

void
ata_io_wait(int io_base) {
	inportb(io_base + ATA_REG_ALTSTATUS);
	inportb(io_base + ATA_REG_ALTSTATUS);
	inportb(io_base + ATA_REG_ALTSTATUS);
	inportb(io_base + ATA_REG_ALTSTATUS);
}

int
ata_wait(int io, int adv)
{
    uint8_t status = 0;

    ata_io_wait(io);

    status = ata_status_wait(io, -1);

    if (adv) {
        status = inportb(io + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return 1;
        if (status & ATA_SR_DF)  return 1;
        if (!(status & ATA_SR_DRQ)) return 1;
    }

    return 0;
}

int
ata_read_one_sector_pio(char *buf, size_t lba)
{
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;

    uint8_t cmd = 0xE0;
    int errors = 0;
    uint8_t slavebit = 0x00;

    //printk("ata: lba: %d\n", lba);
try_a:
    outportb(io + ATA_REG_CONTROL, 0x02);

    ata_wait(io, 0);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, 1);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outportb(io + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    if (ata_wait(io, 1)) {
        errors ++;
        if (errors > 4)
            return -EIO;

        goto try_a;
    }

    for (int i = 0; i < 256; i++) {
        uint16_t d = inportw(io + ATA_REG_DATA);
        *(uint16_t *)(buf + i * 2) = d;
    }

    ata_wait(io, 0);
    return 0;
}

int
ata_read_pio(struct device *dev, void *buf, size_t count)
{
    unsigned long pos = dev->pos;
    int rc = 0, read = 0;

    DISABLE_IRQ();

    for (int i = 0; i < count; i++)
    {
        rc = ata_read_one_sector_pio(buf, pos + i);
        if (rc == -EIO)
            return -EIO;
        buf += 512;
        read += 512;
    }
    dev->pos += count;

    ENABLE_IRQ();
    return count;
}

int
ata_read_one_sector_dma(struct ata_dma_priv *adp, char *buf, size_t lba)
{
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;
    /* XXX: io & dr need to be dynamic once multiple ATA devices
     * are implemented
     */

    uint8_t cmd = 0xE0;
    int errors = 0;
    uint8_t slavebit = 0x00;

    ata_wait(io, 0);

    /* set up DMA transfer by sending STOP */
    outportb(adp->adp_busmaster, 0x00);

    /* send the PRDT */
    outportl(adp->adp_busmaster + 0x04, kv2p(adp->adp_dma_prdt));

    /* enable ERR & IRQ status */
    outportb(adp->adp_busmaster + 0x02,
                inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    /* set direction */
    outportb(adp->adp_busmaster, 0x08);

    /* wait till the device is ready */
	while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY)) break;
	}

    //printk("ata: lba: %d\n", lba);
try_a:
    outportb(io + ATA_REG_CONTROL, 0x00);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    ata_io_wait(io);
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, 1);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));

	/* wait again */
    //printk("about to wait 1\n");
    while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
	}
    //printk("finished wait 1\n");

    outportb(io + ATA_REG_COMMAND, ATA_CMD_READ_DMA);

    //printk("about to wait 2\n");
	ata_io_wait(io);
    //printk("finished wait 2\n");

	outportb(adp->adp_busmaster, 0x08 | 0x01);

    //printk("about to wait 3\n");
    while (1) {
		int status = inportb(adp->adp_busmaster + 0x02);
		int dstatus = inportb(io + ATA_REG_STATUS);
		if (!(status & 0x04)) {
			continue;
		}
		if (!(dstatus & ATA_SR_BSY)) {
			break;
		}
	}
    //printk("finished wait 3\n");

    memcpy(buf, adp->adp_dma_area, 512);

    outportb(adp->adp_busmaster+ 0x2, inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    return 0;
}

int
ata_read_sectors_dma(struct ata_dma_priv *adp, char *buf, size_t lba, size_t count)
{
    if (count < 0 || count > 0x7f) {
        mprintk("CRITICAL: reading >0x7f or <0 sectors is not supported\n");
        return -EIO;
    }

    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;
    /* XXX: io & dr need to be dynamic once multiple ATA devices
     * are implemented
     */

    uint8_t cmd = 0xE0;
    int errors = 0;
    uint8_t slavebit = 0x00;

    ata_wait(io, 0);

    /* if the PRDT is set for the same count then reuse */
    if (count != adp->adp_last_count) {
        /* free the previous */
        na_free(4096, adp->adp_dma_area);

        /* allocate new ones */
        adp->adp_dma_area = na_malloc(count * 512, 4096);
        adp->adp_dma_prdt->prdt_offset = kv2p(adp->adp_dma_area);
        adp->adp_dma_prdt->prdt_bytes = count * 512;
        adp->adp_dma_prdt->prdt_last = 0x8000;
    }

    /* set up DMA transfer by sending STOP */
    outportb(adp->adp_busmaster, 0x00);

    /* send the PRDT */
    outportl(adp->adp_busmaster + 0x04, kv2p(adp->adp_dma_prdt));

    /* enable ERR & IRQ status */
    outportb(adp->adp_busmaster + 0x02,
                inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    /* set direction */
    outportb(adp->adp_busmaster, 0x08);

    /* wait till the device is ready */
	while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY)) break;
	}

    //printk("ata: lba: %d\n", lba);
try_a:
    outportb(io + ATA_REG_CONTROL, 0x00);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    ata_io_wait(io);
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, count);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));

	/* wait again */
    //printk("about to wait 1\n");
    while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
	}
    //printk("finished wait 1\n");

    outportb(io + ATA_REG_COMMAND, ATA_CMD_READ_DMA);

    //printk("about to wait 2\n");
	ata_io_wait(io);
    //printk("finished wait 2\n");

	outportb(adp->adp_busmaster, 0x08 | 0x01);

    //printk("about to wait 3\n");
    while (1) {
		int status = inportb(adp->adp_busmaster + 0x02);
		int dstatus = inportb(io + ATA_REG_STATUS);
		if (!(status & 0x04)) {
			continue;
		}
		if (!(dstatus & ATA_SR_BSY)) {
			break;
		}
	}
    //printk("finished wait 3\n");

    memcpy(buf, adp->adp_dma_area, count * 512);

    outportb(adp->adp_busmaster+ 0x2, inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    adp->adp_last_count = count;

    return 0;

}

int
ata_read_dma(struct device *dev, void *buf, size_t count)
{
    unsigned long pos = dev->pos;
    int rc = 0, read = 0;

#ifdef CONFIG_ATA_SECBYSEC
    for (int i = 0; i < count; i++)
    {
        rc = ata_read_one_sector_dma(dev->priv, buf, pos + i);
        if (rc == -EIO)
            return -EIO;
        buf += 512;
        read += 512;
    }
    dev->pos += count;
#else
    rc = ata_read_sectors_dma(dev->priv, buf, pos, count);
    if (rc == -EIO)
        return -EIO;
    dev->pos += count;
#endif

    return count;
}

int
ata_write_sectors_dma(struct ata_dma_priv *adp, char *buf, size_t lba, size_t count)
{
    if (count < 0 || count > 0x7f) {
        mprintk("CRITICAL: writing >0x7f or <0 sectors is not supported\n");
        return -EIO;
    }

    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;
    /* XXX: io & dr need to be dynamic once multiple ATA devices
     * are implemented
     */

    uint8_t cmd = 0xE0;
    int errors = 0;
    uint8_t slavebit = 0x00;

    ata_wait(io, 0);

    /* if the PRDT is set for the same count then reuse */
    if (count != adp->adp_last_count) {
        /* free the previous */
        na_free(4096, adp->adp_dma_area);

        /* allocate new ones */
        adp->adp_dma_area = na_malloc(count * 512, 4096);
        adp->adp_dma_prdt->prdt_offset = kv2p(adp->adp_dma_area);
        adp->adp_dma_prdt->prdt_bytes = count * 512;
        adp->adp_dma_prdt->prdt_last = 0x8000;
    }

    memcpy(adp->adp_dma_area, buf, count * 512);

    /* set up DMA transfer by sending STOP */
    outportb(adp->adp_busmaster, 0x00);

    /* send the PRDT */
    outportl(adp->adp_busmaster + 0x04, kv2p(adp->adp_dma_prdt));

    /* enable ERR & IRQ status */
    outportb(adp->adp_busmaster + 0x02,
                inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    /* set direction */
    outportb(adp->adp_busmaster, inportb(adp->adp_busmaster) & ~8);

    /* wait till the device is ready */
	while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY)) break;
	}

    //printk("ata: lba: %d\n", lba);
try_a:
    outportb(io + ATA_REG_CONTROL, 0x00);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    ata_io_wait(io);
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, count);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));

	/* wait again */
    //printk("about to wait 1\n");
    while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
	}
    //printk("finished wait 1\n");

    outportb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA);

    //printk("about to wait 2\n");
	ata_io_wait(io);
    //printk("finished wait 2\n");

	outportb(adp->adp_busmaster, 0x08 | 0x01);

    //printk("about to wait 3\n");
    while (1) {
		int status = inportb(adp->adp_busmaster + 0x02);
		int dstatus = inportb(io + ATA_REG_STATUS);
		if (!(status & 0x04)) {
			continue;
		}
		if (!(dstatus & ATA_SR_BSY)) {
			break;
		}
	}
    //printk("finished wait 3\n");

    outportb(adp->adp_busmaster+ 0x2, inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    adp->adp_last_count = count;

    return 0;

}

int
ata_write_one_sector_dma(struct ata_dma_priv *adp, char *buf, size_t lba)
{
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;
    /* XXX: io & dr need to be dynamic once multiple ATA devices
     * are implemented
     */

    uint8_t cmd = 0xE0;
    int errors = 0;
    uint8_t slavebit = 0x00;

    ata_wait(io, 0);

    /* set up DMA transfer by sending STOP */
    outportb(adp->adp_busmaster, 0x00);

    /* send the PRDT */
    outportl(adp->adp_busmaster + 0x04, kv2p(adp->adp_dma_prdt));

    /* enable ERR & IRQ status */
    outportb(adp->adp_busmaster + 0x02,
                inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    /* set direction */
    outportb(adp->adp_busmaster, inportb(adp->adp_busmaster) & ~8);

    /* copy our data */
    memcpy(adp->adp_dma_area, buf, 512);

    /* wait till the device is ready */
	while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY)) break;
	}

    //printk("ata: lba: %d\n", lba);
try_a:
    outportb(io + ATA_REG_CONTROL, 0x00);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    ata_io_wait(io);
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, 1);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));

	/* wait again */
    //printk("about to wait 1\n");
    while (1) {
		uint8_t status = inportb(io + ATA_REG_STATUS);
		if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
	}
    //printk("finished wait 1\n");

    outportb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_DMA);

    //printk("about to wait 2\n");
	ata_io_wait(io);
    //printk("finished wait 2\n");

	outportb(adp->adp_busmaster, 0x08 | 0x01);

    //printk("about to wait 3\n");
    while (1) {
		int status = inportb(adp->adp_busmaster + 0x02);
		int dstatus = inportb(io + ATA_REG_STATUS);
		if (!(status & 0x04)) {
			continue;
		}
		if (!(dstatus & ATA_SR_BSY)) {
			break;
		}
	}
    //printk("finished wait 3\n");

    outportb(adp->adp_busmaster+ 0x2, inportb(adp->adp_busmaster + 0x02) | 0x04 | 0x02);

    return 0;
}

int
ata_write_dma(struct device *dev, void *buf, size_t count)
{
    unsigned long pos = dev->pos;
    int rc = 0, read = 0;

#ifdef CONFIG_ATA_SECBYSEC
    for (int i = 0; i < count; i++)
    {
        rc = ata_write_one_sector_dma(dev->priv, buf, pos + i);
        if (rc == -EIO)
            return -EIO;
        buf += 512;
        read += 512;
    }
    dev->pos += count;
#else
    rc = ata_write_sectors_dma(dev->priv, buf, pos, count);
    if (rc == -EIO)
        return -EIO;
    dev->pos += count;
#endif

    return count;
}

int
ata_write_one_sector_pio(uint16_t *buf, size_t lba)
{
    uint16_t io = ATA_PRIMARY_IO;
    uint8_t  dr = ATA_MASTER;

    uint8_t cmd = 0xE0;
    uint8_t slavebit = 0x00;

    outportb(io + ATA_REG_CONTROL, 0x02);

    ata_wait(io, 0);

    outportb(io + ATA_REG_HDDEVSEL, (cmd | (uint8_t)((lba >> 24 & 0x0F))));
    ata_wait(io, 0);
    outportb(io + ATA_REG_FEATURES, 0x00);
    outportb(io + ATA_REG_SECCOUNT0, 1);
    outportb(io + ATA_REG_LBA0, (uint8_t)(lba));
    outportb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    outportb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    outportb(io + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);
    ata_wait(io, 0);

    for (int i = 0; i < 256; i++) {
        outportw(io + ATA_REG_DATA, buf[i]);
        asm volatile("nop; nop; nop");
    }
    outportb(io + 0x07, ATA_CMD_CACHE_FLUSH);

    ata_wait(io, 0);

    return 0;
}

int
ata_write_pio(struct device *dev, void *buf, size_t count)
{
    unsigned long pos = dev->pos;

    DISABLE_IRQ();
    for (int i = 0; i < count; i++)
    {
        ata_write_one_sector_pio(buf, pos + i);
        buf += 512;
        for (int j = 0; j < 1000; j ++)
            ;
    }
    dev->pos += count;
    ENABLE_IRQ();
    return count;
}

int
ata_sync()
{
    return -ENOSYS;
}

int
ide_identify(void)
{
    uint16_t io = 0;
    /* XXX: support multiple ATA devices */
    io = 0x1F0;
    outportb(ATA_PRIMARY_IO + ATA_REG_HDDEVSEL, 0xA0);
    outportb(io + ATA_REG_SECCOUNT0, 0);
    outportb(io + ATA_REG_LBA0, 0);
    outportb(io + ATA_REG_LBA1, 0);
    outportb(io + ATA_REG_LBA2, 0);
    outportb(io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    uint8_t status = inportb(io + ATA_REG_STATUS);
    if (status)
    {
        /* read the IDENTIFY data */
        struct device *dev;
        void *ide_buf = malloc(512);
        if (!ide_buf)
            return -ENOMEM;

        dev = malloc(sizeof(*dev));
        if (!dev) {
            free(ide_buf);
            return -ENOMEM;
        }

        for (int i = 0; i < 256; i++)
            *(uint16_t *)(ide_buf + i*2) = inportw(io + ATA_REG_DATA);

        free(ide_buf);

        dev->read = ata_read_pio;
        dev->write = ata_write_pio;
        dev->pos = 0;
        dev->type = DEV_TYPE_BLOCK;
        dev->subtype = DEV_TYPE_BLOCK_ATA;
        dev->priv = NULL;
        dev->name = "ata";
        device_register(dev);
        ata_devices[n_ata_devices ++] = dev;
        return 1;
    } else {
        printk("ata: IDENTIFY error on b0d0 -> no status\n");
        return 0;
    }
}


/*
 * do_ata_switch_dma - Switch an ATA device to DMA based I/O
 */
void
do_ata_switch_dma(struct device *dev, uint16_t busmaster)
{
    struct ata_dma_priv *adp = malloc(sizeof(*adp));

    adp->adp_busmaster = busmaster;
    adp->adp_dma_area = na_malloc(4096, 4096);
    adp->adp_dma_prdt = malloc(sizeof(struct dma_prdt));
    adp->adp_dma_prdt->prdt_offset = kv2p(adp->adp_dma_area);
    adp->adp_dma_prdt->prdt_bytes = 512;
    adp->adp_dma_prdt->prdt_last = 0x8000;
    adp->adp_last_count = 1;

    mprintk("PRDT (v 0x%x p 0x%x) AREA (v 0x%x p 0x%x)\n",
                adp->adp_dma_prdt, kv2p(adp->adp_dma_prdt),
                adp->adp_dma_area, kv2p(adp->adp_dma_area));

    mprintk("PRDT (v 0x%x p 0x%x) AREA (v 0x%x p 0x%x)\n",
                adp->adp_dma_prdt, kv2p(adp->adp_dma_prdt),
                adp->adp_dma_area, kv2p(adp->adp_dma_area));

    dev->read = ata_read_dma;
    dev->write = ata_write_dma;
    dev->priv = adp;
}

/*
 * ata_switch_dma - Switch all ATA devices to DMA based I/O
 *
 * @busmaster - busmaster I/O port base value
 */
void
ata_switch_dma(uint16_t busmaster)
{
    int i;

    mprintk("DMA mode is deemed possible, switching devices\n");

    for (i = 0; i < n_ata_devices; i ++) {
        struct device *dev = ata_devices[i];
        /* XXX: once multiple ATA devices are supported, the
         * busmaster I/O base needs to be modified
         */
        do_ata_switch_dma(dev, busmaster);
    }
}

void
ata_probe(void)
{
    int devs = 0;
    if (ide_identify() > 0) {
        printk("ata: primary master is online\n");
        devs ++;
    }
    printk("ata: %d devices brought online\n", devs);
}

void
ide_prim_irq(struct pt_regs *r)
{
    return;
}

void
ata_init(void)
{
    printk("ata: using PIO mode, disregarding secondary\n");
    intr_register_hw(0x20 + ATA_PRIMARY_IRQ, ide_prim_irq);
    ata_probe();
}
