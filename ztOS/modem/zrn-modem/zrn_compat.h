/* What Samsung's kernel had and mainline does not, for the modem driver. */
#pragma once

/* the stock S9 kernel's choices (modem/stock-kernel.config) */
#define CONFIG_SOC_EXYNOS9810 1
#define CONFIG_MCU_IPC 1
#define CONFIG_SHM_IPC 1
#define CONFIG_SEC_SIPC_MODEM_IF 1
#define CONFIG_SEC_MODEM_SS310AP 1
#define CONFIG_UMTS_MODEM_SS310AP 1
#define CONFIG_LINK_DEVICE_SHMEM 1
#define CONFIG_LINK_DEVICE_NAPI 1
#define CONFIG_CP_SECURE_BOOT 1
#define CONFIG_CP_PMUCAL 1
/* CONFIG_CP_ZEROCOPY (stock: y) is left off: its receive path builds skbs
 * over the shared memory with __build_skb, which mainline does not export;
 * the copying path is used instead */
#define CONFIG_MODEM_IF_QOS 1
#define CONFIG_MODEM_IF_NET_GRO 1

#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/time64.h>
#include <linux/timekeeping.h>
#include <linux/sched/clock.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>
#include <linux/etherdevice.h>
#include <linux/proc_fs.h>
#include <linux/pm_wakeup.h>
#include <linux/arm-smccc.h>

/* ---- time: the 32-bit timespec family is gone; the 64-bit one stands in */
#define timespec		timespec64
#define getnstimeofday		ktime_get_real_ts64
#define timespec_sub		timespec64_sub
#define timespec_to_ns		timespec64_to_ns
#define time_to_tm		time64_to_tm
#define cpu_clock(cpu)		local_clock()

/* ---- renamed */
#define strlcpy			strscpy
#define PDE_DATA		pde_data
#define random_ether_addr	eth_random_addr
#define del_timer		timer_delete
#define del_timer_sync		timer_delete_sync

/* ---- Android wakelocks, on mainline's wakeup sources */
#define WAKE_LOCK_SUSPEND	0
struct wake_lock {
	struct wakeup_source *ws;
};
static inline void wake_lock_init(struct wake_lock *lock, int type, const char *name)
{
	lock->ws = wakeup_source_register(NULL, name);
}
static inline void wake_lock_destroy(struct wake_lock *lock)
{
	wakeup_source_unregister(lock->ws);
	lock->ws = NULL;
}
static inline void wake_lock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_stay_awake(lock->ws);
}
static inline void wake_unlock(struct wake_lock *lock)
{
	if (lock->ws)
		__pm_relax(lock->ws);
}
static inline void wake_lock_timeout(struct wake_lock *lock, long timeout)
{
	if (lock->ws)
		__pm_wakeup_event(lock->ws, jiffies_to_msecs(timeout));
}
static inline int wake_lock_active(struct wake_lock *lock)
{
	return lock->ws && lock->ws->active;
}

/* ---- Exynos performance requests: this kernel scales no bus or cluster
 * floor on request, so they are accepted and ignored. Mainline's struct
 * pm_qos_request is used as it is. */
#include <linux/pm_qos.h>
#define PM_QOS_CLUSTER0_FREQ_MIN	1
#define PM_QOS_CLUSTER1_FREQ_MIN	2
#define PM_QOS_BUS_THROUGHPUT		3
#define PM_QOS_DEVICE_THROUGHPUT	4
#define pm_qos_add_request(req, class, value)	do { } while (0)
#define pm_qos_update_request(req, value)	do { } while (0)
#define pm_qos_remove_request(req)		do { } while (0)

/* ---- Samsung debug hooks with nothing behind them here */
#define exynos_ss_set_enable(name, en)	do { } while (0)
#define acpm_stop_log()			do { } while (0)
static inline int gpio_export(unsigned int gpio, bool direction_may_change) { return 0; }
static inline int gpio_export_link(struct device *dev, const char *name, unsigned int gpio) { return 0; }

/* ---- the secure monitor: Samsung's call takes a command and three
 * arguments and returns x0 (drivers/soc/samsung/exynos-smc.c) */
static inline int exynos_smc(unsigned long cmd, unsigned long arg1, unsigned long arg2,
		unsigned long arg3)
{
	struct arm_smccc_res res;
	arm_smccc_smc(cmd, arg1, arg2, arg3, 0, 0, 0, 0, &res);
	return (int)res.a0;
}

/* ---- the CP's power sequences (zrn_cp_pmu.c) */
int cal_cp_init(void);
int cal_cp_status(void);
int cal_cp_reset_assert(void);
int cal_cp_reset_release(void);
void cal_cp_active_clear(void);
void cal_cp_reset_req_clear(void);
void cal_cp_enable_dump_pc_no_pg(void);
void cal_cp_disable_dump_pc_no_pg(void);

/* the CP's secure service: modes as in modem_prj.h (enum cp_boot_mode), 3 and 4 read and
 * write CP_CTRL (drivers/soc/samsung/cal-if/pmucal_cp.c uses the same ID) */
#define SMC_ID				0x82000700

#define CONFIG_CP_RAM_LOGGING 1	/* stock: y */

/* ---- networking */
#include <linux/netdevice.h>
/* GRO: mainline flushes it itself when the poll completes */
#define napi_gro_flush(napi, flush_old)	do { } while (0)
#define GRO_DROP			((gro_result_t)-1)	/* gone: never returned */
/* Samsung's kernel told a receive path which NAPI was polling it; the shared
 * memory link has one, which it names here when it is set up */
extern struct napi_struct *zrn_napi;
#define napi_get_current()		zrn_napi

/* ---- hrtimers are set up with their function now */
static inline enum hrtimer_restart zrn_hrtimer_unset(struct hrtimer *timer)
{
	return HRTIMER_NORESTART;
}
#define hrtimer_init(timer, clock, mode)	hrtimer_setup(timer, zrn_hrtimer_unset, clock, mode)

/* Samsung's memblock helper for handing reserved memory back */
#define free_memsize_reserved(base, size)	do { } while (0)

/* see shm_ipc.c */
dma_addr_t shm_zmb_dma_addr(void *v);
