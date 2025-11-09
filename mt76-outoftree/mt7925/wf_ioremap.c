// SPDX-License-Identifier: ISC
// Simple ioremap helpers used while porting MTK vendor flows.

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <asm/io.h>

int wf_ioremap_read(phys_addr_t addr, u32 *val)
{
	void __iomem *virt;

	if (!val)
		return -EINVAL;

	virt = ioremap(addr, 0x10);
	if (!virt) {
		pr_err("mt7925: ioremap read failed for addr %pa\n", &addr);
		return -ENOMEM;
	}

	*val = readl(virt);
	iounmap(virt);

	return 0;
}
EXPORT_SYMBOL_GPL(wf_ioremap_read);

int wf_ioremap_write(phys_addr_t addr, u32 val)
{
	void __iomem *virt;

	virt = ioremap(addr, 0x10);
	if (!virt) {
		pr_err("mt7925: ioremap write failed for addr %pa\n", &addr);
		return -ENOMEM;
	}

	writel(val, virt);
	iounmap(virt);

	return 0;
}
EXPORT_SYMBOL_GPL(wf_ioremap_write);
