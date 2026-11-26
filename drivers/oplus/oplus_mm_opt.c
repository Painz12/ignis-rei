// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPlus / OnePlus MM Smoothness Pack for Linux 4.19 (SM8250 / munch)
 * - dynamic_readahead (fast cold app launch, prevents bg I/O cache thrash)
 * - uxmem_opt (ALLOC_HARDER reserve preference for UX/foreground threads)
 * - proactive_compact + /sys/kernel/oplus_mm/osvelte/common/compact_memory
 * - ERM /sys/kernel/oplus_mm/erm/free_boost_kb (real extra_free_kbytes boost)
 * - dynamic_swappiness & swappiness_para (/proc/oplus_mem/)
 * - sigkill_diagnosis (/proc/oplus_mem/sigkill_reason) & levelprotect
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/node.h>
#include <linux/compaction.h>
#include <linux/swap.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/sched/clock.h>
#include <linux/oplus_sched_assist.h>

extern int extra_free_kbytes;
extern int vm_swappiness;

static int dynamic_readahead_enabled = 1;
static int uxmem_opt_enable = 1;
static int proactive_compact_enabled = 1;
static int dynamic_swappiness_enabled = 1;
static int levelprotect_enabled = 1;
static int memory_monitor_val;
static int free_boost_kb_val;
static int fragmentation_index_val = 0;

static unsigned long last_compact_jiffies;
static struct delayed_work proactive_compact_work;

/* SIGKILL diagnosis ring buffer */
#define SIGKILL_RING_SIZE	16
struct sigkill_entry {
	u64 ts_ns;
	pid_t target_pid;
	uid_t target_uid;
	char target_comm[TASK_COMM_LEN];
	pid_t sender_pid;
	char sender_comm[TASK_COMM_LEN];
};

static DEFINE_SPINLOCK(sigkill_ring_lock);
static struct sigkill_entry sigkill_ring[SIGKILL_RING_SIZE];
static unsigned int sigkill_head;
static unsigned int sigkill_count;

/* Sysfs hierarchy: /sys/kernel/oplus_mm/{erm, osvelte/common} */
static struct kobject *oplus_mm_kobj;
static struct kobject *oplus_mm_erm_kobj;
static struct kobject *oplus_mm_osvelte_kobj;
static struct kobject *oplus_mm_osvelte_common_kobj;

static void set_proc_owner(struct proc_dir_entry *entry)
{
	if (entry)
		proc_set_user(entry, make_kuid(&init_user_ns, 0),
			      make_kgid(&init_user_ns, 1000));
}

/* ---------------- UX Memory Optimization ---------------- */

bool oplus_uxmem_opt_enabled(void)
{
	return READ_ONCE(uxmem_opt_enable) != 0;
}
EXPORT_SYMBOL_GPL(oplus_uxmem_opt_enabled);

/* ---------------- Dynamic Readahead ---------------- */

unsigned long oplus_dynamic_readahead_pages(struct task_struct *p,
					    unsigned long orig_max)
{
	bool is_ux;

	if (!READ_ONCE(dynamic_readahead_enabled) || !orig_max)
		return orig_max;

	/*
	 * Give UX and foreground app launches a generous 512 KB (128-page)
	 * sequential streaming window for fast APK, DEX, and asset loads.
	 * For all other tasks, NEVER throttle below orig_max (128 KB) so
	 * background and newly-spawning apps never choke on tiny 32KB/64KB I/O fragments.
	 */
	is_ux = oplus_is_ux_task(p) || oplus_is_fg_task(p);
	if (is_ux)
		return max(orig_max, 128UL);

	return orig_max;
}
EXPORT_SYMBOL_GPL(oplus_dynamic_readahead_pages);

/* ---------------- Proactive Compaction ---------------- */

static void trigger_async_compaction(int order)
{
	int nid;
	unsigned long now = jiffies;

	/* Rate-limit to at most once every 2 seconds to prevent CPU burn */
	if (time_before(now, READ_ONCE(last_compact_jiffies) + 2 * HZ))
		return;
	WRITE_ONCE(last_compact_jiffies, now);

	if (order < 1 || order > 4)
		order = 2;

	for_each_node_state(nid, N_MEMORY)
		wakeup_kcompactd(NODE_DATA(nid), order, MAX_NR_ZONES - 1);
}

static void proactive_compact_workfn(struct work_struct *work)
{
	if (READ_ONCE(proactive_compact_enabled)) {
		unsigned long total = totalram_pages;
		long avail = si_mem_available();

		/* Gentle order-2 background compaction when RAM gets tight (< 25%) */
		if (total && avail < (long)(total / 4))
			trigger_async_compaction(2);
	}

	schedule_delayed_work(&proactive_compact_work, 30 * HZ);
}

/* ---------------- SIGKILL Diagnosis ---------------- */

void oplus_record_sigkill(struct task_struct *target,
			  struct task_struct *sender)
{
	unsigned long flags;
	struct sigkill_entry *e;

	if (!target)
		return;

	spin_lock_irqsave(&sigkill_ring_lock, flags);
	e = &sigkill_ring[sigkill_head];
	e->ts_ns = local_clock();
	e->target_pid = target->pid;
	e->target_uid = from_kuid_munged(&init_user_ns, task_uid(target));
	strlcpy(e->target_comm, target->comm, sizeof(e->target_comm));
	if (sender) {
		e->sender_pid = sender->pid;
		strlcpy(e->sender_comm, sender->comm, sizeof(e->sender_comm));
	} else {
		e->sender_pid = 0;
		strlcpy(e->sender_comm, "kernel", sizeof(e->sender_comm));
	}
	sigkill_head = (sigkill_head + 1) % SIGKILL_RING_SIZE;
	if (sigkill_count < SIGKILL_RING_SIZE)
		sigkill_count++;
	spin_unlock_irqrestore(&sigkill_ring_lock, flags);
}
EXPORT_SYMBOL_GPL(oplus_record_sigkill);

/* ---------------- Sysfs: /sys/kernel/oplus_mm/ ---------------- */

static ssize_t free_boost_kb_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(free_boost_kb_val));
}

static ssize_t free_boost_kb_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val))
		return -EINVAL;

	/* Clamp safely to 0..256 MB */
	val = clamp(val, 0, 262144);
	WRITE_ONCE(free_boost_kb_val, val);
	extra_free_kbytes = val;
	setup_per_zone_wmarks();

	return count;
}

static struct kobj_attribute free_boost_kb_attr =
	__ATTR(free_boost_kb, 0664, free_boost_kb_show, free_boost_kb_store);

static int erm_min_batch_kb_val = 1024;
static int erm_free_boost_enabled_val = 1;

static ssize_t erm_min_batch_kb_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(erm_min_batch_kb_val));
}

static ssize_t erm_min_batch_kb_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	int val;
	if (kstrtoint(buf, 10, &val) == 0)
		WRITE_ONCE(erm_min_batch_kb_val, val);
	return count;
}

static struct kobj_attribute min_batch_kb_attr =
	__ATTR(min_batch_kb, 0664, erm_min_batch_kb_show, erm_min_batch_kb_store);

static ssize_t erm_free_boost_enabled_show(struct kobject *kobj,
					   struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(erm_free_boost_enabled_val));
}

static ssize_t erm_free_boost_enabled_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	int val;
	if (kstrtoint(buf, 10, &val) == 0)
		WRITE_ONCE(erm_free_boost_enabled_val, !!val);
	return count;
}

static struct kobj_attribute free_boost_enabled_attr =
	__ATTR(free_boost_enabled, 0664, erm_free_boost_enabled_show, erm_free_boost_enabled_store);

static struct attribute *erm_attrs[] = {
	&free_boost_kb_attr.attr,
	&min_batch_kb_attr.attr,
	&free_boost_enabled_attr.attr,
	NULL,
};

static const struct attribute_group erm_attr_group = {
	.attrs = erm_attrs,
};

static ssize_t compact_memory_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(proactive_compact_enabled));
}

static ssize_t compact_memory_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	int val = 1;

	if (kstrtoint(buf, 10, &val) == 0 && val > 0)
		trigger_async_compaction(2);
	return count;
}

static struct kobj_attribute compact_memory_attr =
	__ATTR(compact_memory, 0664, compact_memory_show, compact_memory_store);

static int enable_bad_proc_val;

static ssize_t enable_bad_proc_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(enable_bad_proc_val));
}

static ssize_t enable_bad_proc_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int val;

	if (kstrtoint(buf, 10, &val) == 0)
		WRITE_ONCE(enable_bad_proc_val, !!val);
	return count;
}

static struct kobj_attribute enable_bad_proc_attr =
	__ATTR(enable_bad_proc, 0664, enable_bad_proc_show, enable_bad_proc_store);

static struct attribute *osvelte_common_attrs[] = {
	&compact_memory_attr.attr,
	&enable_bad_proc_attr.attr,
	NULL,
};

static const struct attribute_group osvelte_common_attr_group = {
	.attrs = osvelte_common_attrs,
};

/* ---------------- Procfs: /proc/oplus_mem/ ---------------- */

static ssize_t mm_int_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	int *val = PDE_DATA(file_inode(file));
	char kbuf[24];
	int len = scnprintf(kbuf, sizeof(kbuf), "%d\n", val ? READ_ONCE(*val) : 0);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t mm_int_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	int *val = PDE_DATA(file_inode(file));
	char kbuf[32];
	int v;

	if (!val || count == 0)
		return count;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (kstrtoint(strim(kbuf), 10, &v) == 0)
		WRITE_ONCE(*val, v);
	return count;
}

static const struct file_operations mm_int_fops = {
	.read = mm_int_read,
	.write = mm_int_write,
	.llseek = generic_file_llseek,
};

static ssize_t swappiness_para_read(struct file *file, char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[32];
	int len = scnprintf(kbuf, sizeof(kbuf), "%d\n", READ_ONCE(vm_swappiness));

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t swappiness_para_write(struct file *file, const char __user *buf,
				     size_t count, loff_t *ppos)
{
	char kbuf[64];
	int val = 0;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (READ_ONCE(dynamic_swappiness_enabled) && sscanf(kbuf, "%d", &val) == 1)
		WRITE_ONCE(vm_swappiness, clamp(val, 0, 100));
	return count;
}

static const struct file_operations swappiness_para_fops = {
	.read = swappiness_para_read,
	.write = swappiness_para_write,
	.llseek = generic_file_llseek,
};

static ssize_t proactive_compact_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	char kbuf[32];
	int v;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (kstrtoint(strim(kbuf), 10, &v) == 0) {
		WRITE_ONCE(proactive_compact_enabled, !!v);
		if (v > 0)
			trigger_async_compaction(2);
	}
	return count;
}

static const struct file_operations proactive_compact_fops = {
	.read = mm_int_read,
	.write = proactive_compact_write,
	.llseek = generic_file_llseek,
};

static int sigkill_reason_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned int i, cnt, start;

	spin_lock_irqsave(&sigkill_ring_lock, flags);
	cnt = sigkill_count;
	start = (cnt == SIGKILL_RING_SIZE) ? sigkill_head : 0;
	for (i = 0; i < cnt; i++) {
		struct sigkill_entry *e = &sigkill_ring[(start + i) % SIGKILL_RING_SIZE];
		seq_printf(m, "ts=%llu target=%s(%d) uid=%u sender=%s(%d)\n",
			   e->ts_ns, e->target_comm, e->target_pid,
			   e->target_uid, e->sender_comm, e->sender_pid);
	}
	spin_unlock_irqrestore(&sigkill_ring_lock, flags);
	return 0;
}

static int sigkill_reason_open(struct inode *inode, struct file *file)
{
	return single_open(file, sigkill_reason_show, NULL);
}

static const struct file_operations sigkill_reason_fops = {
	.open = sigkill_reason_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};


/* ---------------- OPlus BSP ZRAM Opt Sysfs Support ---------------- */
static struct kobject *oplus_bsp_zram_opt_kobj;
static struct kobject *oplus_bsp_zram_opt_params_kobj;
static int direct_swappiness_val = 100;
static int zram_opt_enable_val = 1;

static ssize_t zram_opt_param_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	if (!strcmp(attr->attr.name, "vm_swappiness"))
		return scnprintf(buf, PAGE_SIZE, "%d\n", vm_swappiness);
	if (!strcmp(attr->attr.name, "direct_swappiness"))
		return scnprintf(buf, PAGE_SIZE, "%d\n", direct_swappiness_val);
	if (!strcmp(attr->attr.name, "zram_opt_enable"))
		return scnprintf(buf, PAGE_SIZE, "%d\n", zram_opt_enable_val);
	return scnprintf(buf, PAGE_SIZE, "0\n");
}

static ssize_t zram_opt_param_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int val;
	if (kstrtoint(buf, 10, &val) == 0) {
		if (!strcmp(attr->attr.name, "vm_swappiness"))
			WRITE_ONCE(vm_swappiness, clamp(val, 0, 100));
		else if (!strcmp(attr->attr.name, "direct_swappiness"))
			WRITE_ONCE(direct_swappiness_val, clamp(val, 0, 100));
		else if (!strcmp(attr->attr.name, "zram_opt_enable"))
			WRITE_ONCE(zram_opt_enable_val, !!val);
	}
	return count;
}

static struct kobj_attribute attr_vm_swappiness = __ATTR(vm_swappiness, 0664, zram_opt_param_show, zram_opt_param_store);
static struct kobj_attribute attr_direct_swappiness = __ATTR(direct_swappiness, 0664, zram_opt_param_show, zram_opt_param_store);
static struct kobj_attribute attr_zram_opt_enable = __ATTR(zram_opt_enable, 0664, zram_opt_param_show, zram_opt_param_store);

static struct attribute *zram_opt_attrs[] = {
	&attr_vm_swappiness.attr,
	&attr_direct_swappiness.attr,
	&attr_zram_opt_enable.attr,
	NULL,
};

static struct attribute_group zram_opt_attr_group = {
	.attrs = zram_opt_attrs,
};

static int __init oplus_mm_opt_init(void)
{
	struct proc_dir_entry *dir_mem;
	int rc;

	/* /proc/oplus_mem/ */
	dir_mem = proc_mkdir("oplus_mem", NULL);
	if (dir_mem) {
		set_proc_owner(dir_mem);
		set_proc_owner(proc_create_data("memory_monitor", 0666, dir_mem,
						&mm_int_fops, &memory_monitor_val));
		set_proc_owner(proc_create_data("dynamic_readahead", 0666, dir_mem,
						&mm_int_fops, &dynamic_readahead_enabled));
		set_proc_owner(proc_create_data("alloc_adjust_ctrl", 0666, dir_mem,
						&mm_int_fops, &uxmem_opt_enable));
		set_proc_owner(proc_create_data("dynamic_swappiness", 0666, dir_mem,
						&mm_int_fops, &dynamic_swappiness_enabled));
		set_proc_owner(proc_create("swappiness_para", 0666, dir_mem,
					   &swappiness_para_fops));
		set_proc_owner(proc_create_data("proactive_compact", 0666, dir_mem,
						&proactive_compact_fops, &proactive_compact_enabled));
		set_proc_owner(proc_create_data("levelprotect", 0666, dir_mem,
						&mm_int_fops, &levelprotect_enabled));
		set_proc_owner(proc_create("sigkill_reason", 0444, dir_mem,
					   &sigkill_reason_fops));
		set_proc_owner(proc_create_data("fragmentation_index", 0666, dir_mem,
						&mm_int_fops, &fragmentation_index_val));
	}

	/* /sys/kernel/oplus_mm/erm/free_boost_kb & osvelte/common/compact_memory */
	oplus_mm_kobj = kobject_create_and_add("oplus_mm", kernel_kobj);
	if (oplus_mm_kobj) {
		oplus_mm_erm_kobj = kobject_create_and_add("erm", oplus_mm_kobj);
		if (oplus_mm_erm_kobj) {
			rc = sysfs_create_group(oplus_mm_erm_kobj, &erm_attr_group);
			if (rc)
				pr_warn("oplus_mm_opt: erm group failed (%d)\n", rc);
		}

		oplus_mm_osvelte_kobj = kobject_create_and_add("osvelte", oplus_mm_kobj);
		if (oplus_mm_osvelte_kobj) {
			oplus_mm_osvelte_common_kobj =
				kobject_create_and_add("common", oplus_mm_osvelte_kobj);
			if (oplus_mm_osvelte_common_kobj) {
				rc = sysfs_create_group(oplus_mm_osvelte_common_kobj,
							&osvelte_common_attr_group);
				if (rc)
					pr_warn("oplus_mm_opt: osvelte/common failed (%d)\n", rc);
			}
		}
	}

		/* /sys/module/oplus_bsp_zram_opt/parameters/ */
	if (module_kset)
		oplus_bsp_zram_opt_kobj = kobject_create_and_add("oplus_bsp_zram_opt", &module_kset->kobj);
	if (oplus_bsp_zram_opt_kobj) {
		oplus_bsp_zram_opt_params_kobj = kobject_create_and_add("parameters", oplus_bsp_zram_opt_kobj);
		if (oplus_bsp_zram_opt_params_kobj) {
			rc = sysfs_create_group(oplus_bsp_zram_opt_params_kobj, &zram_opt_attr_group);
			if (rc)
				pr_warn("oplus_mm_opt: zram_opt group failed (%d)\n", rc);
		}
	}

	INIT_DELAYED_WORK(&proactive_compact_work, proactive_compact_workfn);
	schedule_delayed_work(&proactive_compact_work, 60 * HZ);

	pr_info("oplus_mm_opt: initialized (dynamic_readahead, uxmem_opt, proactive_compact, ERM, sigkill_reason)\n");
	return 0;
}

static void __exit oplus_mm_opt_exit(void)
{
	cancel_delayed_work_sync(&proactive_compact_work);
	if (oplus_bsp_zram_opt_params_kobj) {
		sysfs_remove_group(oplus_bsp_zram_opt_params_kobj, &zram_opt_attr_group);
		kobject_put(oplus_bsp_zram_opt_params_kobj);
	}
	if (oplus_bsp_zram_opt_kobj)
		kobject_put(oplus_bsp_zram_opt_kobj);


	if (oplus_mm_osvelte_common_kobj) {
		sysfs_remove_group(oplus_mm_osvelte_common_kobj, &osvelte_common_attr_group);
		kobject_put(oplus_mm_osvelte_common_kobj);
	}
	if (oplus_mm_osvelte_kobj)
		kobject_put(oplus_mm_osvelte_kobj);
	if (oplus_mm_erm_kobj) {
		sysfs_remove_group(oplus_mm_erm_kobj, &erm_attr_group);
		kobject_put(oplus_mm_erm_kobj);
	}
	if (oplus_mm_kobj)
		kobject_put(oplus_mm_kobj);
}

module_init(oplus_mm_opt_init);
module_exit(oplus_mm_opt_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OPlus MM Smoothness Pack (Readahead, UXMem, Proactive Compact, ERM, SIGKILL Diagnosis)");
