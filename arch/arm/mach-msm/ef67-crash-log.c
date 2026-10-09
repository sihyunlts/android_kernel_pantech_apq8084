/*
 * IM-A930S S0226114 stock bootloader crash log ABI.
 *
 * Deliberately separate from normal EF67 support: remove this option and its
 * board hook to drop the diagnostic handoff. No dump mode is enabled here.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <asm/cacheflush.h>
#include <asm/memory.h>
#include <asm/outercache.h>
#include <asm/system_misc.h>
#include <mach/ef67-crash-log.h>
#include <soc/qcom/smem.h>

#define EF67_IMEM_MAGIC_OFFSET	0xff8
#define EF67_IMEM_REASON_OFFSET	0xffc
#define EF67_IMEM_MAGIC		0xdead0525
#define EF67_REASON_FLAGS	0x00ffff00
#define EF67_REASON_ABNORMAL	0xda000031
#define EF67_REASON_PANIC	0xda000011
#define EF67_REASON_REBOOT	0xda0000f1
#define EF67_PANIC_MAGIC	0xdaeddaed

/* All addresses in this ABI are 32-bit physical addresses, including on CAF. */
struct ef67_crash_header {
	u32 magic;
	u32 version;
	u32 klog_address;
	u32 klog_size;
	u32 android_log_buffers[9];
	char mmu_script[512];
	u32 mmu_script_size;
	u32 debug_address;
	u32 debug_size;
};

static struct ef67_crash_header *crash_header;
static void __iomem *restart_imem;

static void ef67_set_reset_reason(u32 reason)
{
	u32 flags = readl_relaxed(restart_imem + EF67_IMEM_REASON_OFFSET);

	/* Keep the firmware's RAM/USB dump selections and other mode flags. */
	writel_relaxed((flags & EF67_REASON_FLAGS) | reason,
		       restart_imem + EF67_IMEM_REASON_OFFSET);
	wmb();
}

static int ef67_crash_panic(struct notifier_block *nb, unsigned long event,
			    void *data)
{
	u32 control, ttbr0, domain, ttbr1, prrr, nmrr;
	char *buffer = log_buf_addr_get();
	u32 size = log_buf_len_get();
	phys_addr_t address = __pa(buffer);

	asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r" (control));
	asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r" (ttbr0));
	asm volatile("mrc p15, 0, %0, c3, c0, 0" : "=r" (domain));
	asm volatile("mrc p15, 0, %0, c2, c0, 1" : "=r" (ttbr1));
	asm volatile("mrc p15, 0, %0, c10, c2, 0" : "=r" (prrr));
	asm volatile("mrc p15, 0, %0, c10, c2, 1" : "=r" (nmrr));
	crash_header->mmu_script_size = scnprintf(crash_header->mmu_script,
		sizeof(crash_header->mmu_script),
		"PER.S C15:0x1 %%LONG 0x%X\n"
		"PER.S C15:0x2 %%LONG 0x%X\n"
		"PER.S C15:0x3 %%LONG 0x%X\n"
		"PER.S C15:0x102 %%LONG 0x%X\n"
		"PER.S C15:0x2A %%LONG 0x%X\n"
		"PER.S C15:0x12A %%LONG 0x%X\n"
		"MMU.SCAN\nMMU.ON\n\n\n\n",
		control, ttbr0, domain, ttbr1, prrr, nmrr);
	crash_header->version = 1;
	wmb();
	crash_header->magic = EF67_PANIC_MAGIC;

	/* The next bootloader reads DDR directly, without the kernel's caches. */
	__cpuc_flush_dcache_area(buffer, size);
	outer_flush_range(address, address + size);
	ef67_set_reset_reason(EF67_REASON_PANIC);
	return NOTIFY_DONE;
}

static struct notifier_block ef67_panic_notifier = {
	.notifier_call = ef67_crash_panic,
};

static int ef67_crash_reboot(struct notifier_block *nb, unsigned long event,
			     void *data)
{
	if (event == SYS_RESTART)
		ef67_set_reset_reason(EF67_REASON_REBOOT);
	return NOTIFY_DONE;
}

static struct notifier_block ef67_reboot_notifier = {
	.notifier_call = ef67_crash_reboot,
};

void __init ef67_crash_log_init(void)
{
	struct device_node *node;
	struct resource resource;
	u32 board_id[2];
	unsigned int size;
	phys_addr_t log_address = __pa(log_buf_addr_get());
	int ret;

	BUILD_BUG_ON(sizeof(struct ef67_crash_header) != 0x240);
	BUILD_BUG_ON(offsetof(struct ef67_crash_header, mmu_script) != 0x34);
	BUILD_BUG_ON(offsetof(struct ef67_crash_header, mmu_script_size) != 0x234);

	/* The S stock ABI has not been verified against an L bootloader. */
	node = of_find_node_by_path("/");
	ret = of_property_read_u32_array(node, "qcom,board-id", board_id, 2);
	of_node_put(node);
	if (ret || board_id[0] != 0x1a3b || board_id[1] != 3)
		return;

	if (log_address > UINT_MAX) {
		pr_err("ef67-crash-log: log address exceeds stock ABI\n");
		return;
	}
	crash_header = smem_get_entry(SMEM_ID_VENDOR2, &size, 0,
				     SMEM_ANY_HOST_FLAG);
	if (!crash_header || size < sizeof(*crash_header)) {
		crash_header = NULL;
		pr_err("ef67-crash-log: stock crash header unavailable\n");
		return;
	}

	node = of_find_compatible_node(NULL, NULL, "qcom,msm-imem");
	ret = node ? of_address_to_resource(node, 0, &resource) : -ENODEV;
	if (!ret && resource_size(&resource) >= 0x1000)
		restart_imem = of_iomap(node, 0);
	of_node_put(node);
	if (!restart_imem) {
		pr_err("ef67-crash-log: restart memory unavailable\n");
		return;
	}
	if (readl_relaxed(restart_imem + EF67_IMEM_MAGIC_OFFSET) !=
	    EF67_IMEM_MAGIC) {
		pr_err("ef67-crash-log: stock restart signature unavailable\n");
		iounmap(restart_imem);
		restart_imem = NULL;
		return;
	}

	/* Do not leave stale Android logger pointers from the preceding kernel. */
	memset(crash_header, 0, sizeof(*crash_header));
	crash_header->klog_address = (u32)log_address;
	crash_header->klog_size = log_buf_len_get();
	wmb();
	ret = register_reboot_notifier(&ef67_reboot_notifier);
	if (ret) {
		pr_err("ef67-crash-log: reboot notifier failed: %d\n", ret);
		iounmap(restart_imem);
		restart_imem = NULL;
		return;
	}
	atomic_notifier_chain_register(&panic_notifier_list,
				       &ef67_panic_notifier);
	ef67_set_reset_reason(EF67_REASON_ABNORMAL);
#ifdef CONFIG_DEBUG_USER
	/* Stock fastboot omits image-header user_debug command-line options. */
	user_debug |= UDBG_SEGV;
#endif
	pr_info("ef67-crash-log: stock kernel log handoff ready (%u bytes)\n",
		log_buf_len_get());
}
