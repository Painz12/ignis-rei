/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OPlus / OnePlus Sched Assist, Frame Boost (120Hz), FG UID & MM Opt hooks
 * for Linux 4.19 (SM8250 / munch) — Android 11-17 / ColorOS / OxygenOS
 */
#ifndef _LINUX_OPLUS_SCHED_ASSIST_H
#define _LINUX_OPLUS_SCHED_ASSIST_H

#include <linux/types.h>
#include <linux/sched.h>
#include <linux/cred.h>

/* UX task type bitmask stored in lower bits of task->oplus_ux_state */
#define SA_TYPE_LIGHT		(1U << 0)
#define SA_TYPE_HEAVY		(1U << 1)
#define SA_TYPE_ANIMATOR	(1U << 2)
#define SA_TYPE_LISTPICK	(1U << 3)
#define SA_TYPE_INHERIT		(1U << 4)
#define SA_TYPE_FBG		(1U << 5)
#define SA_TYPE_MASK		(SA_TYPE_LIGHT | SA_TYPE_HEAVY | \
				 SA_TYPE_ANIMATOR | SA_TYPE_LISTPICK | \
				 SA_TYPE_INHERIT | SA_TYPE_FBG)

/* Inherit source bitmask stored in upper bits of task->oplus_ux_state */
enum oplus_inherit_ux_type {
	INHERIT_UX_BINDER = 0,
	INHERIT_UX_RWSEM,
	INHERIT_UX_MUTEX,
	INHERIT_UX_FUTEX,
	INHERIT_UX_MAX,
};

#define SA_INHERIT_SHIFT	16
#define SA_INHERIT_BIT(type)	(1U << (SA_INHERIT_SHIFT + (type)))
#define SA_INHERIT_MASK		(((1U << INHERIT_UX_MAX) - 1U) << SA_INHERIT_SHIFT)

#ifdef CONFIG_OPLUS_SCHED_ASSIST

extern int oplus_sched_assist_enabled;
extern int oplus_binder_ux_enable;
extern int oplus_binder_async_ux_enable;
extern int oplus_fbg_enabled;
extern int oplus_fbg_slide_enabled;
extern int oplus_fbg_slide_min_util;
extern int oplus_fbg_min_util;
extern int oplus_uclamp_boost_min;

static inline bool oplus_is_ux_task(struct task_struct *p)
{
	if (!p || !READ_ONCE(oplus_sched_assist_enabled))
		return false;
	return (READ_ONCE(p->oplus_ux_state) & (SA_TYPE_MASK | SA_INHERIT_MASK)) != 0;
}

static inline bool oplus_test_inherit_ux(struct task_struct *p, int type)
{
	if (!p || type < 0 || type >= INHERIT_UX_MAX)
		return false;
	return (READ_ONCE(p->oplus_ux_state) & SA_INHERIT_BIT(type)) != 0;
}

static inline bool oplus_binder_ux_enabled(void)
{
	return READ_ONCE(oplus_sched_assist_enabled) &&
	       READ_ONCE(oplus_binder_ux_enable);
}

static inline bool oplus_binder_async_ux_enabled(void)
{
	return oplus_binder_ux_enabled() &&
	       READ_ONCE(oplus_binder_async_ux_enable);
}

void oplus_set_ux_state(struct task_struct *p, u32 state);
void oplus_set_inherit_ux(struct task_struct *p, int type);
void oplus_unset_inherit_ux(struct task_struct *p, int type);
unsigned long oplus_ux_min_util(struct task_struct *p);
bool oplus_is_fg_uid(uid_t uid);
bool oplus_is_fg_task(struct task_struct *p);
void oplus_sched_assist_fork(struct task_struct *p);

#else /* !CONFIG_OPLUS_SCHED_ASSIST */

static inline bool oplus_is_ux_task(struct task_struct *p) { return false; }
static inline bool oplus_test_inherit_ux(struct task_struct *p, int type) { return false; }
static inline bool oplus_binder_ux_enabled(void) { return false; }
static inline bool oplus_binder_async_ux_enabled(void) { return false; }
static inline void oplus_set_ux_state(struct task_struct *p, u32 state) { }
static inline void oplus_set_inherit_ux(struct task_struct *p, int type) { }
static inline void oplus_unset_inherit_ux(struct task_struct *p, int type) { }
static inline unsigned long oplus_ux_min_util(struct task_struct *p) { return 0; }
static inline bool oplus_is_fg_uid(uid_t uid) { return false; }
static inline bool oplus_is_fg_task(struct task_struct *p) { return false; }
static inline void oplus_sched_assist_fork(struct task_struct *p) { }

#endif /* CONFIG_OPLUS_SCHED_ASSIST */

#ifdef CONFIG_OPLUS_MM_OPT
bool oplus_uxmem_opt_enabled(void);
unsigned long oplus_dynamic_readahead_pages(struct task_struct *p,
					    unsigned long orig_max);
void oplus_record_sigkill(struct task_struct *target,
			  struct task_struct *sender);
#else
static inline bool oplus_uxmem_opt_enabled(void) { return false; }
static inline unsigned long oplus_dynamic_readahead_pages(struct task_struct *p,
							  unsigned long orig_max)
{
	return orig_max;
}
static inline void oplus_record_sigkill(struct task_struct *target,
					struct task_struct *sender) { }
#endif /* CONFIG_OPLUS_MM_OPT */

#endif /* _LINUX_OPLUS_SCHED_ASSIST_H */
