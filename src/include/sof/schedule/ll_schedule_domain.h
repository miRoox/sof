/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Tomasz Lauda <tomasz.lauda@linux.intel.com>
 */

#ifndef __SOF_SCHEDULE_LL_SCHEDULE_DOMAIN_H__
#define __SOF_SCHEDULE_LL_SCHEDULE_DOMAIN_H__

#include <sof/atomic.h>
#include <sof/debug/panic.h>
#include <sof/lib/alloc.h>
#include <sof/lib/cpu.h>
#include <sof/lib/clk.h>
#include <sof/lib/memory.h>
#include <sof/sof.h>
#include <sof/spinlock.h>
#include <sof/trace/trace.h>
#include <ipc/topology.h>
#include <user/trace.h>
#include <stdbool.h>
#include <stdint.h>

#define LL_TIMER_PERIOD_US 1000ULL /* default period in microseconds */

struct dma;
struct ll_schedule_domain;
struct task;
struct timer;

struct ll_schedule_domain_ops {
	int (*domain_register)(struct ll_schedule_domain *domain,
			       struct task *task,
			       void (*handler)(void *arg), void *arg);
	int (*domain_unregister)(struct ll_schedule_domain *domain,
				 struct task *task, uint32_t num_tasks);
	void (*domain_enable)(struct ll_schedule_domain *domain, int core);
	void (*domain_disable)(struct ll_schedule_domain *domain, int core);
	void (*domain_set)(struct ll_schedule_domain *domain, uint64_t start);
	void (*domain_clear)(struct ll_schedule_domain *domain);
	bool (*domain_is_pending)(struct ll_schedule_domain *domain,
				  struct task *task, struct comp_dev **comp);
};

struct ll_schedule_domain {
	uint64_t next_tick;		/**< ticks just set for next run */
	uint64_t new_target_tick;	/**< for the next set, used during the reschedule stage */
	spinlock_t lock;		/**< standard lock */
	atomic_t total_num_tasks;	/**< total number of registered tasks */
	atomic_t enabled_cores;		/**< number of enabled cores */
	uint32_t ticks_per_ms;		/**< number of clock ticks per ms */
	int type;			/**< domain type */
	int clk;			/**< source clock */
	bool synchronous;		/**< are tasks should be synchronous */
	bool full_sync;			/**< tasks should be full synchronous, no time dependent */
	void *priv_data;		/**< pointer to private data */
	bool registered[CONFIG_CORE_COUNT];		/**< registered cores */
	bool enabled[CONFIG_CORE_COUNT];		/**< enabled cores */
	const struct ll_schedule_domain_ops *ops;	/**< domain ops */
};

#define ll_sch_domain_set_pdata(domain, data) ((domain)->priv_data = (data))

#define ll_sch_domain_get_pdata(domain) ((domain)->priv_data)

static inline struct ll_schedule_domain *timer_domain_get(void)
{
	return sof_get()->platform_timer_domain;
}

static inline struct ll_schedule_domain *dma_domain_get(void)
{
	return sof_get()->platform_dma_domain;
}

static inline struct ll_schedule_domain *domain_init
				(int type, int clk, bool synchronous,
				 const struct ll_schedule_domain_ops *ops)
{
	struct ll_schedule_domain *domain;

	domain = rballoc(0, SOF_MEM_CAPS_RAM | SOF_MEM_CAPS_DMA, sizeof(*domain));
	domain->type = type;
	domain->clk = clk;
	domain->synchronous = synchronous;
	domain->full_sync = false;
	domain->ticks_per_ms = clock_ms_to_ticks(clk, 1);
	domain->ops = ops;
	/* maximum value means no tick has been set to timer */
	domain->next_tick = UINT64_MAX;
	domain->new_target_tick = UINT64_MAX;

	spinlock_init(&domain->lock);
	atomic_init(&domain->total_num_tasks, 0);
	atomic_init(&domain->enabled_cores, 0);

	return domain;
}

/* configure the next interrupt for domain */
static inline void domain_set(struct ll_schedule_domain *domain, uint64_t start)
{
	if (domain->ops->domain_set)
		domain->ops->domain_set(domain, start);
	else
		domain->next_tick = start;
}

/* clear the interrupt for domain */
static inline void domain_clear(struct ll_schedule_domain *domain)
{
	if (domain->ops->domain_clear)
		domain->ops->domain_clear(domain);

	/* reset to denote no tick/interrupt is set */
	domain->next_tick = UINT64_MAX;
}

static inline int domain_register(struct ll_schedule_domain *domain,
				  struct task *task,
				  void (*handler)(void *arg), void *arg)
{
	int core = cpu_get_id();
	int ret;

	assert(domain->ops->domain_register);

	ret = domain->ops->domain_register(domain, task, handler, arg);

	if (!ret) {
		/* registered one more task, increase the count */
		atomic_add(&domain->total_num_tasks, 1);

		if (!domain->registered[core])
			/* first task of the core, new client registered */
			domain->registered[core] = true;
	}

	return ret;
}

static inline void domain_unregister(struct ll_schedule_domain *domain,
				     struct task *task, uint32_t num_tasks)
{
	int core = cpu_get_id();
	bool registered = domain->registered[core];
	int ret;

	assert(domain->ops->domain_unregister);

	/* unregistering a task, decrement the count */
	atomic_sub(&domain->total_num_tasks, 1);

	/* the last task of the core, unregister the client/core */
	if (!num_tasks && registered)
		domain->registered[core] = false;

	/*
	 * In some cases .domain_unregister() might not return, terminating the
	 * current thread, that's why we had to update state before calling it.
	 */
	ret = domain->ops->domain_unregister(domain, task, num_tasks);
	if (ret < 0) {
		/* Failed to unregister the domain, restore state */
		atomic_add(&domain->total_num_tasks, 1);

		if (!num_tasks && domain->registered[core])
			domain->registered[core] = registered;
	}
}

static inline void domain_enable(struct ll_schedule_domain *domain, int core)
{
	if (!domain->enabled[core] && domain->ops->domain_enable) {
		domain->ops->domain_enable(domain, core);
		domain->enabled[core] = true;
		atomic_add(&domain->enabled_cores, 1);
	}
}

static inline void domain_disable(struct ll_schedule_domain *domain, int core)
{
	if (domain->enabled[core] && domain->ops->domain_disable) {
		domain->ops->domain_disable(domain, core);
		domain->enabled[core] = false;
		atomic_sub(&domain->enabled_cores, 1);
	}
}

static inline bool domain_is_pending(struct ll_schedule_domain *domain,
				     struct task *task, struct comp_dev **comp)
{
	bool ret;

	assert(domain->ops->domain_is_pending);

	ret = domain->ops->domain_is_pending(domain, task, comp);

	return ret;
}


#ifndef __ZEPHYR__
struct ll_schedule_domain *timer_domain_init(struct timer *timer, int clk);
#else
struct ll_schedule_domain *zephyr_domain_init(struct timer *timer, int clk);
#define timer_domain_init zephyr_domain_init
#endif

struct ll_schedule_domain *dma_multi_chan_domain_init(struct dma *dma_array,
						      uint32_t num_dma, int clk,
						      bool aggregated_irq);

struct ll_schedule_domain *dma_single_chan_domain_init(struct dma *dma_array,
						       uint32_t num_dma,
						       int clk);

#endif /* __SOF_SCHEDULE_LL_SCHEDULE_DOMAIN_H__ */
