/*
 * sgiwd93.c: SGI WD93 scsi driver.
 *
 * Copyright (C) 1996 David S. Miller (dm@engr.sgi.com)
 *		 1999 Andrew R. Baker (andrewb@uab.edu)
 *		      - Support for 2nd SCSI controller on Indigo2
 *		 2001 Florian Lohoff (flo@rfc822.org)
 *		      - Delete HPC scatter gather (Read corruption on 
 *		        multiple disks)
 *		      - Cleanup wback cache handling
 * 
 * (In all truth, Jed Schimmel wrote all this code.)
 *
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/blk.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/sgialib.h>
#include <asm/sgi/sgi.h>
#include <asm/sgi/mc.h>
#include <asm/sgi/hpc3.h>
#include <asm/sgi/ip22.h>
#include <asm/irq.h>
#include <asm/io.h>

#include "scsi.h"
#include "hosts.h"
#include "wd33c93.h"
#include "sgiwd93.h"

#include <linux/stat.h>

#if 0
#define DPRINTK(args...)	printk(args)
#else
#define DPRINTK(args...)
#endif

struct hpc_chunk {
	struct hpc_dma_desc desc;
	u32 _padding;	/* align to quadword boundary */
};

struct Scsi_Host *sgiwd93_host;
struct Scsi_Host *sgiwd93_host1;

/* Wuff wuff, wuff, wd33c93.c, wuff wuff, object oriented, bow wow. */
static inline void write_wd33c93_count(const wd33c93_regs regs,
                                      unsigned long value)
{
	*regs.SASR = WD_TRANSFER_COUNT_MSB;
	mb();
	*regs.SCMD = ((value >> 16) & 0xff);
	*regs.SCMD = ((value >>  8) & 0xff);
	*regs.SCMD = ((value >>  0) & 0xff);
	mb();
}

static inline unsigned long read_wd33c93_count(const wd33c93_regs regs)
{
	unsigned long value;

	*regs.SASR = WD_TRANSFER_COUNT_MSB;
	mb();
	value =  ((*regs.SCMD & 0xff) << 16);
	value |= ((*regs.SCMD & 0xff) <<  8);
	value |= ((*regs.SCMD & 0xff) <<  0);
	mb();
	return value;
}

static void sgiwd93_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned long flags;

	spin_lock_irqsave(&io_request_lock, flags);
	wd33c93_intr((struct Scsi_Host *) dev_id);
	spin_unlock_irqrestore(&io_request_lock, flags);
}

static inline
void fill_hpc_entries(struct hpc_chunk **hcp, char *addr, unsigned long len)
{
	unsigned long physaddr;
	unsigned long count;
	
	physaddr = virt_to_bus(addr);
	while (len) {
		/*
		 * even cntinfo could be up to 16383, without
		 * magic only 8192 works correctly
		 */
		count = len > 8192 ? 8192 : len;
		(*hcp)->desc.pbuf = physaddr;
		(*hcp)->desc.cntinfo = count;
		(*hcp)++;
		len -= count;
		physaddr += count;
	}
}

static int dma_setup(Scsi_Cmnd *cmd, int datainp)
{
	struct WD33C93_hostdata *hdata =
		(struct WD33C93_hostdata *) cmd->host->hostdata;
	struct hpc3_scsiregs *hregs =
		(struct hpc3_scsiregs *) cmd->host->base;
	struct hpc_chunk *hcp = (struct hpc_chunk *) hdata->dma_bounce_buffer;

	DPRINTK("dma_setup: datainp<%d> hcp<%p> ", datainp, hcp);

	hdata->dma_dir = datainp;

	/*
	 * wd33c93 shouldn't pass us bogus dma_setups, but it does:-(  The
	 * other wd33c93 drivers deal with it the same way (which isn't that
	 * obvious).  IMHO a better fix would be, not to do these dma setups
	 * in the first place.
	 */
	if (cmd->SCp.ptr == NULL || cmd->SCp.this_residual == 0)
		return 1;

	fill_hpc_entries(&hcp, cmd->SCp.ptr, cmd->SCp.this_residual);

	/*
	 * To make sure, if we trip an HPC bug, that we transfer every single
	 * byte, we tag on an extra zero length dma descriptor at the end of
	 * the chain.
	 */
	hcp->desc.pbuf = 0;
	hcp->desc.cntinfo = HPCDMA_EOX;

	DPRINTK(" HPCGO\n");

	/* Start up the HPC. */
	hregs->ndptr = virt_to_bus(hdata->dma_bounce_buffer);
	if (datainp) {
		dma_cache_inv((unsigned long) cmd->SCp.ptr,
		              cmd->SCp.this_residual);
		hregs->ctrl = HPC3_SCTRL_ACTIVE;
	} else {
		dma_cache_wback_inv((unsigned long) cmd->SCp.ptr,
		                    cmd->SCp.this_residual);
		hregs->ctrl = HPC3_SCTRL_ACTIVE | HPC3_SCTRL_DIR;
	}

	return 0;
}

static void dma_stop(struct Scsi_Host *instance, Scsi_Cmnd *SCpnt,
		     int status)
{
	struct WD33C93_hostdata *hdata =
		(struct WD33C93_hostdata *) instance->hostdata;
	struct hpc3_scsiregs *hregs;

	if (!SCpnt)
		return;

	hregs = (struct hpc3_scsiregs *) SCpnt->host->base;

	DPRINTK("dma_stop: status<%d> ", status);

	/* First stop the HPC and flush it's FIFO. */
	if (hdata->dma_dir) {
		hregs->ctrl |= HPC3_SCTRL_FLUSH;
		while (hregs->ctrl & HPC3_SCTRL_ACTIVE)
			barrier();
	}
	hregs->ctrl = 0;

	DPRINTK("\n");
}

void sgiwd93_reset(unsigned long base)
{
	struct hpc3_scsiregs *hregs = (struct hpc3_scsiregs *) base;

	hregs->ctrl = HPC3_SCTRL_CRESET;
	udelay(50);
	hregs->ctrl = 0;
}

static inline void init_hpc_chain(uchar *buf)
{
	struct hpc_chunk *hcp = (struct hpc_chunk *) buf;
	unsigned long start, end;

	start = (unsigned long) buf;
	end = start + PAGE_SIZE;
	while (start < end) {
		hcp->desc.pnext = virt_to_bus(hcp + 1);
		hcp->desc.cntinfo = HPCDMA_EOX;
		hcp++;
		start += sizeof(struct hpc_chunk);
	};
	hcp--;
	hcp->desc.pnext = virt_to_bus(buf);

	/* Force flush to memory */
	dma_cache_wback_inv((unsigned long) buf, PAGE_SIZE);
}

static struct Scsi_Host * __init sgiwd93_setup_scsi(
	Scsi_Host_Template *SGIblows, int unit, int irq,
	struct hpc3_scsiregs *hregs, unsigned char *wdregs)
{
	struct WD33C93_hostdata *hdata;
	struct Scsi_Host *host;
	wd33c93_regs regs;
	uchar *buf;

	host = scsi_register(SGIblows, sizeof(struct WD33C93_hostdata));
	if (!host)
		return NULL;

	host->base = (unsigned long) hregs;
	host->irq = irq;

	buf = (uchar *) get_zeroed_page(GFP_KERNEL);
	if (!buf) {
		printk(KERN_WARNING "sgiwd93: Could not allocate memory for "
		       "host %d buffer.\n", unit);
		goto out_unregister;
	}
	init_hpc_chain(buf);

	regs.SASR = wdregs + 3;
	regs.SCMD = wdregs + 7;

	wd33c93_init(host, regs, dma_setup, dma_stop, WD33C93_FS_16_20);

	hdata = (struct WD33C93_hostdata *) host->hostdata;
	hdata->no_sync = 0;
	hdata->dma_bounce_buffer = (uchar *) KSEG1ADDR(buf);

	if (request_irq(irq, sgiwd93_intr, 0, "SGI WD93", (void *) host)) {
		printk(KERN_WARNING "sgiwd93: Could not register irq %d "
		       "for host %d.\n", irq, unit);
		goto out_free;
	}
	return host;

out_free:
	free_page((unsigned long)buf);
	wd33c93_release();

out_unregister:
	scsi_unregister(host);

	return NULL;
}

int __init sgiwd93_detect(Scsi_Host_Template *SGIblows)
{
	int found = 0;

	SGIblows->proc_name = "SGIWD93";
	sgiwd93_host = sgiwd93_setup_scsi(SGIblows, 0, SGI_WD93_0_IRQ,
	                                  &hpc3c0->scsi_chan0,
	                                  (unsigned char *)hpc3c0->scsi0_ext);
	if (sgiwd93_host)
		found++;

	/* Set up second controller on the Indigo2 */
	if (ip22_is_fullhouse()) {
		sgiwd93_host1 = sgiwd93_setup_scsi(SGIblows, 1, SGI_WD93_1_IRQ,
		                          &hpc3c0->scsi_chan1,
		                          (unsigned char *)hpc3c0->scsi1_ext);
		if (sgiwd93_host1)
			found++;
	}

	return found;
}

#define HOSTS_C

#include "sgiwd93.h"

static Scsi_Host_Template driver_template = SGIWD93_SCSI;

#include "scsi_module.c"

int sgiwd93_release(struct Scsi_Host *instance)
{
#ifdef MODULE
	free_irq(SGI_WD93_0_IRQ, sgiwd93_intr);
	free_page(KSEG0ADDR(hdata->dma_bounce_buffer));
	wd33c93_release();
	if (ip22_is_fullhouse()) {
		free_irq(SGI_WD93_1_IRQ, sgiwd93_intr);
		free_page(KSEG0ADDR(hdata1->dma_bounce_buffer));
		wd33c93_release();
	}
#endif
	return 1;
}
