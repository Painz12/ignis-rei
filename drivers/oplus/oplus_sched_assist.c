// SPDX-License-Identifier: GPL-2.0-only
/*
 * OPlus / OnePlus Sched Assist, Frame Boost (120Hz), FG UID, Binder/Lock UX
 * Inheritance, UCLAMP Control, and SurfaceFlinger Sync Support for Linux 4.19
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/sysctl.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/spinlock.h>
#include <linux/uidgid.h>
#include <linux/user_namespace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/oplus_sched_assist.h>

#define MAX_FG_UIDS		16
#define MAX_UX_INHERIT_DEPTH	16

/* Core exported controls */
int oplus_sched_assist_enabled = 1;
int oplus_binder_ux_enable = 1;
int oplus_binder_async_ux_enable = 1;
int oplus_fbg_enabled = 1;
int oplus_fbg_debug = 0;
int oplus_fbg_input_enabled = 1;
int oplus_fbg_slide_enabled = 1;
int oplus_fbg_slide_min_util = 350;
int oplus_fbg_min_util = 384;
int oplus_uclamp_boost_min = 0;

/* Runtime state */
static int sa_scene;
static int sa_sched_prop_val;
static int sa_opt_enable = 1;
static int sa_inherit_ux = 1;
static int sa_sched_fbg_opt = 1;
static int sa_fbg_slide_opt = 1;
static int sa_fbg_camera_opt = 1;
static int sa_fbg_anim_opt = 1;
static int sa_v2 = 1;
static int sa_debug_enabled;
static int sa_lowend_plat_opt;
static int sa_disable_camera_ux;
static int sa_control_ux_select;
static int sa_group_enabled = 1;
static int sa_ddl_enabled;
static int sa_audio_status;

/* 120 Hz default frame target: 8,333,333 ns */
static u64 sa_sf_target_time_ns = 8333333ULL;
static int fbg_fps = 120;
static int fbg_level;
static int fbg_mode;
static int fbg_scene;
static int fbg_ctrl_val = 1;
static int game_target_fps = 120;
static int game_pid_val;
static int game_render_pid_val;
static int hfs_ctrl_val = 1;
static int frame_sync_ctrl_val = 1;

static int binder_set_last_async_ux = 1;
static int binder_set_async_after_pending = 1;
static int binder_sched_debug;
static int binder_use_t_vendordata;
static int binder_unset_async_inrestore = 1;

/* Foreground UID tracking */
static DEFINE_SPINLOCK(fg_uid_lock);
static uid_t fg_uids[MAX_FG_UIDS];
static int fg_uid_count;

static void set_proc_owner(struct proc_dir_entry *entry)
{
	if (entry)
		proc_set_user(entry, make_kuid(&init_user_ns, 0),
			      make_kgid(&init_user_ns, 1000));
}

/* ---------------- Core UX & Inheritance API ---------------- */

void oplus_sched_assist_fork(struct task_struct *p)
{
	if (!p)
		return;
	WRITE_ONCE(p->oplus_ux_state, 0);
	WRITE_ONCE(p->oplus_ux_depth, 0);
}

void oplus_set_ux_state(struct task_struct *p, u32 state)
{
	u32 cur, next;

	if (!p)
		return;
	cur = READ_ONCE(p->oplus_ux_state);
	next = (cur & SA_INHERIT_MASK) | (state & SA_TYPE_MASK);
	WRITE_ONCE(p->oplus_ux_state, next);
}
EXPORT_SYMBOL_GPL(oplus_set_ux_state);

void oplus_set_inherit_ux(struct task_struct *p, int type)
{
	u32 cur, next, depth;

	if (!p || !READ_ONCE(oplus_sched_assist_enabled) || !READ_ONCE(sa_inherit_ux))
		return;
	if (type < 0 || type >= INHERIT_UX_MAX)
		return;
	/* Avoid inheriting onto kernel idle/swapper threads */
	if (p->pid == 0)
		return;

	depth = READ_ONCE(p->oplus_ux_depth);
	if (depth >= MAX_UX_INHERIT_DEPTH)
		return;

	cur = READ_ONCE(p->oplus_ux_state);
	next = cur | SA_TYPE_INHERIT | SA_INHERIT_BIT(type);
	WRITE_ONCE(p->oplus_ux_state, next);
	WRITE_ONCE(p->oplus_ux_depth, depth + 1);
}
EXPORT_SYMBOL_GPL(oplus_set_inherit_ux);

void oplus_unset_inherit_ux(struct task_struct *p, int type)
{
	u32 cur, next, depth;

	if (!p || type < 0 || type >= INHERIT_UX_MAX)
		return;

	cur = READ_ONCE(p->oplus_ux_state);
	if (!(cur & SA_INHERIT_BIT(type)))
		return;

	next = cur & ~SA_INHERIT_BIT(type);
	if (!(next & SA_INHERIT_MASK))
		next &= ~SA_TYPE_INHERIT;

	WRITE_ONCE(p->oplus_ux_state, next);

	depth = READ_ONCE(p->oplus_ux_depth);
	if (depth > 0)
		WRITE_ONCE(p->oplus_ux_depth, depth - 1);
}
EXPORT_SYMBOL_GPL(oplus_unset_inherit_ux);

bool oplus_is_fg_uid(uid_t uid)
{
	int i, cnt;
	uid_t appid;

	if (uid == 0)
		return false;

	cnt = READ_ONCE(fg_uid_count);
	if (cnt <= 0)
		return false;
	if (cnt > MAX_FG_UIDS)
		cnt = MAX_FG_UIDS;

	appid = (uid >= 10000) ? (uid % 100000) : uid;

	for (i = 0; i < cnt; i++) {
		uid_t fg = READ_ONCE(fg_uids[i]);
		if (fg == uid)
			return true;
		/* Match secondary users / clone apps (e.g. user 999 in ColorOS multi-app) */
		if (fg >= 10000 && (fg % 100000) == appid)
			return true;
	}
	return false;
}
EXPORT_SYMBOL_GPL(oplus_is_fg_uid);

bool oplus_is_fg_task(struct task_struct *p)
{
	uid_t uid;

	if (!p)
		return false;
	uid = from_kuid_munged(&init_user_ns, task_uid(p));
	return oplus_is_fg_uid(uid);
}
EXPORT_SYMBOL_GPL(oplus_is_fg_task);

static inline bool is_critical_ui_thread(struct task_struct *p)
{
	/* Ultra-fast 1-byte reject for 99% of tasks */
	char c0 = p->comm[0];

	if (c0 == 'R' && !strncmp(p->comm, "RenderThread", 12))
		return true;
	if (c0 == 's' && !strncmp(p->comm, "surfaceflinger", 14))
		return true;
	if (c0 == 'S' && !strncmp(p->comm, "SurfaceFlinger", 14))
		return true;
	if (c0 == 'H' && !strncmp(p->comm, "HwComposer", 10))
		return true;
	if (c0 == 'c' && !strncmp(p->comm, "composer", 8))
		return true;
	if (c0 == 'c' && !strncmp(p->comm, "cr_renderer", 11))
		return true;
	if (c0 == 'C' && !strncmp(p->comm, "Chrome_InProcRe", 15))
		return true;
	if (c0 == 'U' && !strncmp(p->comm, "UnityMain", 9))
		return true;
	return false;
}

unsigned long oplus_ux_min_util(struct task_struct *p)
{
	u32 state;
	unsigned long util = 0;
	int uclamp_hint;

	if (!p || !READ_ONCE(oplus_sched_assist_enabled))
		return 0;

	state = READ_ONCE(p->oplus_ux_state);
	if (!state && p->group_leader)
		state = READ_ONCE(p->group_leader->oplus_ux_state);
	if (!state) {
		/*
		 * Even if userspace didn't tag SurfaceFlinger/RenderThread yet
		 * (e.g. AOSP/HyperOS), provide light 120Hz responsiveness when
		 * frame_boost is enabled.
		 */
		if (READ_ONCE(oplus_fbg_enabled) && is_critical_ui_thread(p))
			util = 256;
		else
			return 0;
	} else {
		if (state & SA_TYPE_FBG) {
			unsigned long fbg_u = READ_ONCE(oplus_fbg_min_util);
			int lvl = READ_ONCE(fbg_level);

			if (READ_ONCE(sa_sf_target_time_ns) <= 9000000ULL)
				fbg_u = max(fbg_u, 384UL);
			if (lvl > 0)
				fbg_u = min(768UL, fbg_u + (unsigned long)lvl * 64UL);
			util = max(util, fbg_u);
		}
		if ((state & (SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK)) &&
		    READ_ONCE(oplus_fbg_slide_enabled)) {
			unsigned long slide_u = clamp(READ_ONCE(oplus_fbg_slide_min_util), 200, 768);
			util = max(util, slide_u);
		}
		if (state & SA_TYPE_HEAVY)
			util = max(util, 320UL);
		if (state & (SA_TYPE_LIGHT | SA_TYPE_INHERIT | SA_INHERIT_MASK))
			util = max(util, 256UL);
	}

	/* Apply active scene boost (app launch / slide) only to UX threads */
	if (util > 0 && READ_ONCE(sa_scene) > 0 && READ_ONCE(oplus_fbg_slide_enabled)) {
		unsigned long slide_u = clamp(READ_ONCE(oplus_fbg_slide_min_util), 256, 768);
		util = max(util, slide_u);
	}

	uclamp_hint = READ_ONCE(oplus_uclamp_boost_min);
	if (util > 0 && uclamp_hint > 0)
		util = max(util, (unsigned long)clamp(uclamp_hint, 0, 1024));

	return min(util, 1024UL);
}
EXPORT_SYMBOL_GPL(oplus_ux_min_util);

/* Helper: tag a PID with a UX bitmask */
static void tag_pid_ux(pid_t pid, u32 flags, bool clear_if_zero)
{
	struct task_struct *task;

	if (pid <= 0)
		return;

	rcu_read_lock();
	task = find_task_by_vpid(pid);
	if (task) {
		if (flags == 0 && clear_if_zero)
			oplus_set_ux_state(task, 0);
		else
			oplus_set_ux_state(task,
				(READ_ONCE(task->oplus_ux_state) & SA_TYPE_MASK) | flags);
	}
	rcu_read_unlock();
}

/* ---------------- Generic proc helpers ---------------- */

static ssize_t sa_int_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	int *val = PDE_DATA(file_inode(file));
	char kbuf[24];
	int len = scnprintf(kbuf, sizeof(kbuf), "%d\n", val ? READ_ONCE(*val) : 0);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t sa_int_write(struct file *file, const char __user *buf,
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

static long sa_dummy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return 0;
}

static const struct file_operations sa_int_fops = {
	.read = sa_int_read,
	.write = sa_int_write,
	.unlocked_ioctl = sa_dummy_ioctl,
	.compat_ioctl = sa_dummy_ioctl,
	.llseek = generic_file_llseek,
};

/* ---------------- sf_target_time (120 Hz default) ---------------- */

static ssize_t sf_target_time_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	char kbuf[32];
	int len = scnprintf(kbuf, sizeof(kbuf), "%llu\n",
			    READ_ONCE(sa_sf_target_time_ns));
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t sf_target_time_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[32];
	u64 ns;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';
	if (kstrtou64(strim(kbuf), 10, &ns) == 0 && ns >= 4000000ULL && ns <= 50000000ULL) {
		WRITE_ONCE(sa_sf_target_time_ns, ns);
		if (ns <= 9000000ULL)
			WRITE_ONCE(fbg_fps, 120);
		else if (ns <= 12000000ULL)
			WRITE_ONCE(fbg_fps, 90);
		else
			WRITE_ONCE(fbg_fps, 60);
	}
	return count;
}

static const struct file_operations sf_target_time_fops = {
	.read = sf_target_time_read,
	.write = sf_target_time_write,
	.unlocked_ioctl = sa_dummy_ioctl,
	.compat_ioctl = sa_dummy_ioctl,
	.llseek = generic_file_llseek,
};

/* ---------------- UX Task Tagging (/proc/oplus_scheduler/sched_assist/ux_task etc.) ---------------- */

static ssize_t ux_task_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char kbuf[64];
	int pid = 0, type = SA_TYPE_HEAVY;
	int n;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	/* Accept formats: "pid", "pid state", "p pid state", "t pid state" */
	n = sscanf(kbuf, "%d %d", &pid, &type);
	if (n <= 0) {
		char tag;
		n = sscanf(kbuf, "%c %d %d", &tag, &pid, &type);
		if (n < 2)
			return count;
		if (n == 2)
			type = SA_TYPE_HEAVY;
	} else if (n == 1) {
		type = SA_TYPE_HEAVY;
	}

	if (pid > 0)
		tag_pid_ux(pid, (u32)(type & SA_TYPE_MASK), true);
	return count;
}

static ssize_t ux_task_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos)
{
	return simple_read_from_buffer(buf, count, ppos, "0\n", 2);
}

static const struct file_operations ux_task_fops = {
	.read = ux_task_read,
	.write = ux_task_write,
	.unlocked_ioctl = sa_dummy_ioctl,
	.compat_ioctl = sa_dummy_ioctl,
	.llseek = generic_file_llseek,
};

/* ---------------- FBG / Game PID Tagging ---------------- */

static ssize_t fbg_pid_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	int *store = PDE_DATA(file_inode(file));
	char kbuf[64];
	int pid = 0;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	if (sscanf(kbuf, "%d", &pid) == 1) {
		if (store)
			WRITE_ONCE(*store, pid);
		if (pid > 0)
			tag_pid_ux(pid, SA_TYPE_FBG | SA_TYPE_HEAVY, false);
	}
	return count;
}

static const struct file_operations fbg_pid_fops = {
	.read = sa_int_read,
	.write = fbg_pid_write,
	.unlocked_ioctl = sa_dummy_ioctl,
	.compat_ioctl = sa_dummy_ioctl,
	.llseek = generic_file_llseek,
};

/* ---------------- FG UID (/proc/fg_uid/fg_uids) ---------------- */

static ssize_t fg_uid_read(struct file *file, char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[256];
	int len = 0, i;
	unsigned long flags;

	spin_lock_irqsave(&fg_uid_lock, flags);
	if (fg_uid_count == 0) {
		len = scnprintf(kbuf, sizeof(kbuf), "0\n");
	} else {
		for (i = 0; i < fg_uid_count && len < (int)sizeof(kbuf) - 16; i++)
			len += scnprintf(kbuf + len, sizeof(kbuf) - len,
					 "%u%s", fg_uids[i],
					 (i + 1 < fg_uid_count) ? " " : "\n");
	}
	spin_unlock_irqrestore(&fg_uid_lock, flags);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t fg_uid_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	char kbuf[128];
	char *p, *tok;
	uid_t parsed[MAX_FG_UIDS];
	int cnt = 0;
	unsigned long flags;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	p = kbuf;
	while ((tok = strsep(&p, " ,\t\n")) != NULL && cnt < MAX_FG_UIDS) {
		unsigned int uid;
		if (!*tok)
			continue;
		if (kstrtouint(tok, 10, &uid) == 0 && uid > 0)
			parsed[cnt++] = (uid_t)uid;
	}

	spin_lock_irqsave(&fg_uid_lock, flags);
	memcpy(fg_uids, parsed, cnt * sizeof(uid_t));
	WRITE_ONCE(fg_uid_count, cnt);
	spin_unlock_irqrestore(&fg_uid_lock, flags);

	return count;
}

static const struct file_operations fg_uid_fops = {
	.read = fg_uid_read,
	.write = fg_uid_write,
	.llseek = generic_file_llseek,
};

/* ---------------- /proc/oplus_uc/oplus_uclamp_ctrl ---------------- */

static ssize_t oplus_uclamp_ctrl_read(struct file *file, char __user *buf,
				      size_t count, loff_t *ppos)
{
	char kbuf[32];
	int len = scnprintf(kbuf, sizeof(kbuf), "%d\n",
			    READ_ONCE(oplus_uclamp_boost_min));
	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static ssize_t oplus_uclamp_ctrl_write(struct file *file, const char __user *buf,
				       size_t count, loff_t *ppos)
{
	char kbuf[64];
	int a = 0, b = 0, c = 0, n;

	if (count == 0)
		return 0;
	if (count >= sizeof(kbuf))
		count = sizeof(kbuf) - 1;
	if (copy_from_user(kbuf, buf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	/* Accept "<min_util>" or "<pid> <min_util> [max_util]" */
	n = sscanf(kbuf, "%d %d %d", &a, &b, &c);
	if (n == 1) {
		WRITE_ONCE(oplus_uclamp_boost_min, clamp(a, 0, 1024));
	} else if (n >= 2) {
		if (a > 0 && b > 0)
			tag_pid_ux(a, SA_TYPE_HEAVY, false);
		else if (a > 0 && b == 0)
			tag_pid_ux(a, 0, true);
		WRITE_ONCE(oplus_uclamp_boost_min, clamp(b, 0, 1024));
	}
	return count;
}

static const struct file_operations oplus_uclamp_ctrl_fops = {
	.read = oplus_uclamp_ctrl_read,
	.write = oplus_uclamp_ctrl_write,
	.unlocked_ioctl = sa_dummy_ioctl,
	.compat_ioctl = sa_dummy_ioctl,
	.llseek = generic_file_llseek,
};

/* ---------------- /proc/oplus_binder/all_tasks_ux_sts ---------------- */

static int all_tasks_ux_sts_show(struct seq_file *m, void *v)
{
	struct task_struct *g, *p;
	int ux_cnt = 0, inh_cnt = 0;

	rcu_read_lock();
	for_each_process_thread(g, p) {
		u32 st = READ_ONCE(p->oplus_ux_state);
		if (st) {
			ux_cnt++;
			if (st & SA_INHERIT_MASK)
				inh_cnt++;
			if (ux_cnt <= 32)
				seq_printf(m, "pid=%d comm=%s state=0x%x depth=%u\n",
					   p->pid, p->comm, st,
					   READ_ONCE(p->oplus_ux_depth));
		}
	}
	rcu_read_unlock();

	seq_printf(m, "total_ux=%d inherited_ux=%d\n", ux_cnt, inh_cnt);
	return 0;
}

static int all_tasks_ux_sts_open(struct inode *inode, struct file *file)
{
	return single_open(file, all_tasks_ux_sts_show, NULL);
}

static const struct file_operations all_tasks_ux_sts_fops = {
	.open = all_tasks_ux_sts_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/* ---------------- /proc/sys/fbg/ sysctl ---------------- */

#define FBG_SYSCTL_INT(name, var) {			\
		.procname	= name,			\
		.data		= &var,			\
		.maxlen		= sizeof(int),		\
		.mode		= 0666,			\
		.proc_handler	= proc_dointvec,	\
	}

static struct ctl_table fbg_sysctl_table[] = {
	FBG_SYSCTL_INT("frame_boost_enabled", oplus_fbg_enabled),
	FBG_SYSCTL_INT("frame_boost_debug", oplus_fbg_debug),
	FBG_SYSCTL_INT("input_boost_enabled", oplus_fbg_input_enabled),
	FBG_SYSCTL_INT("slide_boost_enabled", oplus_fbg_slide_enabled),
	FBG_SYSCTL_INT("slide_min_util", oplus_fbg_slide_min_util),
	{ }
};

static struct ctl_table sched_assist_kernel_table[] = {
	FBG_SYSCTL_INT("sched_assist_scene", sa_scene),
	{ }
};

static struct ctl_table fbg_sysctl_root[] = {
	{
		.procname	= "fbg",
		.mode		= 0555,
		.child		= fbg_sysctl_table,
	},
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= sched_assist_kernel_table,
	},
	{ }
};

static struct ctl_table_header *fbg_sysctl_header;
static int dummy_zero;

static void create_int_node(const char *name, umode_t mode,
			    struct proc_dir_entry *parent, int *val)
{
	set_proc_owner(proc_create_data(name, mode, parent, &sa_int_fops, val));
}

static int __init oplus_sched_assist_init(void)
{
	struct proc_dir_entry *dir_sched, *dir_sa, *dir_audio;
	struct proc_dir_entry *dir_fbg, *dir_binder, *dir_fg, *dir_uc;
	struct proc_dir_entry *dir_game, *dir_fsync;

	/* /proc/fg_uid/fg_uids */
	dir_fg = proc_mkdir("fg_uid", NULL);
	if (dir_fg) {
		set_proc_owner(dir_fg);
		set_proc_owner(proc_create("fg_uids", 0666, dir_fg, &fg_uid_fops));
	}

	/* /proc/oplus_uc/oplus_uclamp_ctrl */
	dir_uc = proc_mkdir("oplus_uc", NULL);
	if (dir_uc) {
		set_proc_owner(dir_uc);
		set_proc_owner(proc_create("oplus_uclamp_ctrl", 0666, dir_uc,
					   &oplus_uclamp_ctrl_fops));
	}

	/* /proc/oplus_scheduler/sched_assist/ */
	dir_sched = proc_mkdir("oplus_scheduler", NULL);
	if (dir_sched) {
		set_proc_owner(dir_sched);
		dir_sa = proc_mkdir("sched_assist", dir_sched);
		if (dir_sa) {
			set_proc_owner(dir_sa);
			create_int_node("sched_assist_enabled", 0666, dir_sa, &oplus_sched_assist_enabled);
			create_int_node("sched_assist_scene", 0666, dir_sa, &sa_scene);
			create_int_node("sched_prop", 0666, dir_sa, &sa_sched_prop_val);
			set_proc_owner(proc_create("sched_impt_task", 0666, dir_sa, &ux_task_fops));
			set_proc_owner(proc_create("im_flag_app", 0666, dir_sa, &ux_task_fops));
			set_proc_owner(proc_create("im_flag", 0666, dir_sa, &ux_task_fops));
			set_proc_owner(proc_create("ux_task", 0666, dir_sa, &ux_task_fops));
			set_proc_owner(proc_create("ux_task_app", 0666, dir_sa, &ux_task_fops));
			set_proc_owner(proc_create("fg_uid", 0666, dir_sa, &fg_uid_fops));
			create_int_node("opt_enable", 0666, dir_sa, &sa_opt_enable);
			create_int_node("frame_boost", 0666, dir_sa, &oplus_fbg_enabled);
			set_proc_owner(proc_create("sf_target_time", 0666, dir_sa, &sf_target_time_fops));
			create_int_node("inherit_ux", 0666, dir_sa, &sa_inherit_ux);
			set_proc_owner(proc_create("heavy_task", 0666, dir_sa, &ux_task_fops));
			create_int_node("sched_fbg_opt", 0666, dir_sa, &sa_sched_fbg_opt);
			create_int_node("fbg_slide_opt", 0666, dir_sa, &sa_fbg_slide_opt);
			create_int_node("fbg_camera_opt", 0666, dir_sa, &sa_fbg_camera_opt);
			create_int_node("fbg_anim_opt", 0666, dir_sa, &sa_fbg_anim_opt);
			create_int_node("sched_assist_v2", 0666, dir_sa, &sa_v2);
			create_int_node("debug_enabled", 0666, dir_sa, &sa_debug_enabled);
			create_int_node("lowend_plat_opt", 0666, dir_sa, &sa_lowend_plat_opt);
			create_int_node("sched_disable_camera_ux", 0666, dir_sa, &sa_disable_camera_ux);
			create_int_node("sched_control_ux_select", 0666, dir_sa, &sa_control_ux_select);
			create_int_node("sched_group_enabled", 0666, dir_sa, &sa_group_enabled);
			create_int_node("sched_ddl_enabled", 0666, dir_sa, &sa_ddl_enabled);

			dir_audio = proc_mkdir("audio", dir_sa);
			if (dir_audio) {
				set_proc_owner(dir_audio);
				create_int_node("status", 0666, dir_audio, &sa_audio_status);
			}
		}
	}

	/* /proc/oplus_frame_boost/ */
	dir_fbg = proc_mkdir("oplus_frame_boost", NULL);
	if (dir_fbg) {
		set_proc_owner(dir_fbg);
		create_int_node("ctrl", 0666, dir_fbg, &fbg_ctrl_val);
		create_int_node("sys_ctrl", 0666, dir_fbg, &fbg_ctrl_val);
		create_int_node("fbg_ctrl", 0666, dir_fbg, &fbg_ctrl_val);
		create_int_node("fbg_fps", 0666, dir_fbg, &fbg_fps);
		create_int_node("fbg_level", 0666, dir_fbg, &fbg_level);
		set_proc_owner(proc_create_data("fbg_pid", 0666, dir_fbg, &fbg_pid_fops, &game_pid_val));
		set_proc_owner(proc_create_data("fbg_render_pid", 0666, dir_fbg, &fbg_pid_fops, &game_render_pid_val));
		create_int_node("fbg_mode", 0666, dir_fbg, &fbg_mode);
		create_int_node("fbg_scene", 0666, dir_fbg, &fbg_scene);
	}

	/* /proc/oplus_binder/ */
	dir_binder = proc_mkdir("oplus_binder", NULL);
	if (dir_binder) {
		set_proc_owner(dir_binder);
		create_int_node("ux_flag", 0666, dir_binder, &oplus_binder_ux_enable);
		create_int_node("async_ux_enable", 0666, dir_binder, &oplus_binder_async_ux_enable);
		create_int_node("set_last_async_ux", 0666, dir_binder, &binder_set_last_async_ux);
		create_int_node("set_async_ux_after_pending", 0666, dir_binder, &binder_set_async_after_pending);
		set_proc_owner(proc_create("all_tasks_ux_sts", 0444, dir_binder, &all_tasks_ux_sts_fops));
		create_int_node("sched_debug", 0666, dir_binder, &binder_sched_debug);
		create_int_node("use_t_vendordata", 0666, dir_binder, &binder_use_t_vendordata);
		create_int_node("unset_async_ux_inrestore", 0666, dir_binder, &binder_unset_async_inrestore);
		create_int_node("get_random_binder_task", 0666, dir_binder, &dummy_zero);
	}

	/* /proc/game_opt/ + ColorOS 17 SurfaceFlinger hfs_ctrl & frame_sync/sync_ctrl */
	dir_game = proc_mkdir("game_opt", NULL);
	if (dir_game) {
		set_proc_owner(dir_game);
		set_proc_owner(proc_create_data("render_thread", 0666, dir_game, &fbg_pid_fops, &game_render_pid_val));
		set_proc_owner(proc_create_data("game_pid", 0666, dir_game, &fbg_pid_fops, &game_pid_val));
		create_int_node("cpu_load", 0666, dir_game, &dummy_zero);
		create_int_node("target_fps", 0666, dir_game, &game_target_fps);
		create_int_node("heavy_task_info", 0444, dir_game, &dummy_zero);
		create_int_node("rt_info", 0664, dir_game, &dummy_zero);
		create_int_node("rt_num", 0444, dir_game, &dummy_zero);
		create_int_node("cpu_min_freq", 0664, dir_game, &dummy_zero);
		create_int_node("cpu_max_freq", 0664, dir_game, &dummy_zero);
		create_int_node("disable_cpufreq_limit", 0664, dir_game, &dummy_zero);
		create_int_node("fake_cpu7_cpuinfo_max_freq", 0664, dir_game, &dummy_zero);
		create_int_node("skip_gameself_setaffinity", 0664, dir_game, &dummy_zero);
		create_int_node("hfs_ctrl", 0666, dir_game, &hfs_ctrl_val);

		dir_fsync = proc_mkdir("frame_sync", dir_game);
		if (dir_fsync) {
			set_proc_owner(dir_fsync);
			create_int_node("sync_ctrl", 0666, dir_fsync, &frame_sync_ctrl_val);
		}
	}

	fbg_sysctl_header = register_sysctl_table(fbg_sysctl_root);

	pr_info("oplus_sched_assist: initialized (120Hz frame_boost, binder/lock UX inherit, fg_uid, oplus_uc)\n");
	return 0;
}

static void __exit oplus_sched_assist_exit(void)
{
	if (fbg_sysctl_header)
		unregister_sysctl_table(fbg_sysctl_header);
}

module_init(oplus_sched_assist_init);
module_exit(oplus_sched_assist_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("OPlus Sched Assist, 120Hz Frame Boost, Binder/Lock UX & SurfaceFlinger Sync");
