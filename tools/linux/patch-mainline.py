#!/usr/bin/env python3
"""Starbite's own edits to the mainline tree (run by build-kernel.sh before the
port's build; idempotent, each edit checked against its anchor).

1. dw_mmc: "starbite,dma-32bit". The S9's microSD controller (DW MSHC 2.70a)
   reports 64-bit IDMAC addressing in HCON and needs the 64-bit descriptor
   format, but its transfers failed with the descriptor list in the high RAM
   bank (0x8_8000_0000): the stock DT uses desc-size 4 (32-bit). Keep the
   64-bit descriptors, cap the DMA mask at 32 bits (swiotlb bounces the rest).
"""
import sys
from pathlib import Path

src = Path(sys.argv[1])


def edit(rel, anchor, new, marker):
    p = src / rel
    s = p.read_text()
    if marker in s:
        print(f'{rel}: already patched')
        return
    if s.count(anchor) != 1:
        sys.exit(f'{rel}: anchor not found exactly once')
    p.write_text(s.replace(anchor, new))
    print(f'{rel}: patched')


edit('drivers/mmc/host/dw_mmc.c',
     '''			if (dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(64)))
				dev_info(host->dev, "Fail to set 64-bit DMA mask");''',
     '''			if (dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(64)))
				dev_info(host->dev, "Fail to set 64-bit DMA mask");
			/* starbite,dma-32bit: 64-bit descriptors, 32-bit reach */
			if (device_property_read_bool(host->dev, "starbite,dma-32bit") &&
			    !dma_set_mask_and_coherent(host->dev, DMA_BIT_MASK(32)))
				dev_info(host->dev, "DMA limited to 32-bit addresses\\n");''',
     'starbite,dma-32bit')

# 2. dw_mmc: say why a transfer failed (bring-up; the stock messages are
#    dev_dbg and this kernel has no dynamic debug). First 20 errors only.
edit('drivers/mmc/host/dw_mmc.c',
     '''	else if (status & SDMMC_INT_RESP_ERR)
		cmd->error = -EIO;
	else
		cmd->error = 0;
''',
     '''	else if (status & SDMMC_INT_RESP_ERR)
		cmd->error = -EIO;
	else
		cmd->error = 0;

	{	/* starbite: command errors */
		static int starbite_cmd_errs;
		if (cmd->error && starbite_cmd_errs++ < 20)
			dev_err(host->dev, "starbite: CMD%u arg 0x%08x error %d rintsts 0x%08x resp 0x%08x\\n",
				cmd->opcode, cmd->arg, cmd->error, status, cmd->resp[0]);
	}
''',
     'starbite: CMD%u')
edit('drivers/mmc/host/dw_mmc.c',
     '''		dev_dbg(host->dev, "data error, status 0x%08x\\n", status);''',
     '''		{	/* starbite: data errors with the IDMAC status */
			static int starbite_data_errs;
			if (starbite_data_errs++ < 20)
				dev_err(host->dev, "starbite: data error, rintsts 0x%08x idsts64 0x%08x dir %d blocks %u x %u\\n",
					status, host->dma_64bit_address ? mci_readl(host, IDSTS64) : mci_readl(host, IDSTS),
					host->dir_status, data->blocks, data->blksz);
		}''',
     'starbite: data error')

# 3. dw_mmc: "starbite,max-data-timeout". The vendor driver always writes
#    TMOUT = ~0 on this controller; with mainline's per-request value (card
#    clocks) every read ended in DRTO (rintsts 0x20c), even at 400 kHz:
#    the counter evidently runs faster than the card clock here.
edit('drivers/mmc/host/dw_mmc.c',
     '''	if (drv_data && drv_data->set_data_timeout)
		return drv_data->set_data_timeout(host, timeout_ns);
''',
     '''	if (drv_data && drv_data->set_data_timeout)
		return drv_data->set_data_timeout(host, timeout_ns);

	if (device_property_read_bool(host->dev, "starbite,max-data-timeout")) {
		mci_writel(host, TMOUT, 0xFFFFFFFF);
		return;
	}
''',
     'starbite,max-data-timeout')
