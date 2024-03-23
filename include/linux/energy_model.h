/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_ENERGY_MODEL_H
#define _LINUX_ENERGY_MODEL_H
#include <linux/cpumask.h>
#include <linux/jump_label.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/topology.h>
#include <linux/types.h>

#ifdef CONFIG_ENERGY_MODEL
/**
 * em_cap_state - Capacity state of a performance domain
 * @frequency:	The CPU frequency in KHz, for consistency with CPUFreq
 * @power:	The power consumed by 1 CPU at this level, in milli-watts
 * @cost:	The cost coefficient associated with this level, used during
 *		energy calculation. Equal to: power * max_frequency / frequency
 */
// 性能域容量状态
struct em_cap_state {
	// CPU 的频率，单位为千赫兹
	unsigned long frequency;
	// 在该频率下的功耗，单位是毫瓦（mW）
	unsigned long power;
	// 该频率下的能效系数，通常 cost = power x max_frequency / frequency 
	unsigned long cost;
};

/**
 * em_perf_domain - Performance domain
 * @table:		List of capacity states, in ascending order
 * @nr_cap_states:	Number of capacity states
 * @cpus:		Cpumask covering the CPUs of the domain
 *
 * A "performance domain" represents a group of CPUs whose performance is
 * scaled together. All CPUs of a performance domain must have the same
 * micro-architecture. Performance domains often have a 1-to-1 mapping with
 * CPUFreq policies.
 */
// 用于描述性能域
struct em_perf_domain {
	// table 指表述 CPU 频率和功耗之间关系的一个表
	struct em_cap_state *table;
	// 表示 CPU 有多少个频率点或者能力点
	int nr_cap_states;
	// cpus 表示这个性能域所包含的 CPU 位图
	unsigned long cpus[0];
};

#define EM_CPU_MAX_POWER 0xFFFF

struct em_data_callback {
	/**
	 * active_power() - Provide power at the next capacity state of a CPU
	 * @power	: Active power at the capacity state in mW (modified)
	 * @freq	: Frequency at the capacity state in kHz (modified)
	 * @cpu		: CPU for which we do this operation
	 *
	 * active_power() must find the lowest capacity state of 'cpu' above
	 * 'freq' and update 'power' and 'freq' to the matching active power
	 * and frequency.
	 *
	 * The power is the one of a single CPU in the domain, expressed in
	 * milli-watts. It is expected to fit in the [0, EM_CPU_MAX_POWER]
	 * range.
	 *
	 * Return 0 on success.
	 */
	// 用于获取 OPP 表里的频率值（单位千赫兹）和计算好的功耗值（单位是毫瓦）
	// 还函数会从 OPP 表的最低的频率开始往上查找，最后频率和功耗值会通过指针来呈现
	int (*active_power)(unsigned long *power, unsigned long *freq, int cpu);
};
#define EM_DATA_CB(_active_power_cb) { .active_power = &_active_power_cb }

struct em_perf_domain *em_cpu_get(int cpu);
int em_register_perf_domain(cpumask_t *span, unsigned int nr_states,
						struct em_data_callback *cb);

/**
 * em_pd_energy() - Estimates the energy consumed by the CPUs of a perf. domain
 * @pd		: performance domain for which energy has to be estimated
 * @max_util	: highest utilization among CPUs of the domain
 * @sum_util	: sum of the utilization of all CPUs in the domain
 *
 * Return: the sum of the energy consumed by the CPUs of the domain assuming
 * a capacity state satisfying the max utilization of the domain.
 */
// 用于预测一个性能域的功耗情况
// 参数 pd 表示将要预测哪个性能域的功耗情况
// 参数 max_util 表示在这个性能域中所有的 CPU 里面最高的 CPU 使用率
// 参数 sum_util 表示所有 CPU 的总 CPU 利用率
static inline unsigned long em_pd_energy(struct em_perf_domain *pd,
				unsigned long max_util, unsigned long sum_util)
{
	unsigned long freq, scale_cpu;
	struct em_cap_state *cs;
	int i, cpu;

	/*
	 * In order to predict the capacity state, map the utilization of the
	 * most utilized CPU of the performance domain to a requested frequency,
	 * like schedutil.
	 */
	cpu = cpumask_first(to_cpumask(pd->cpus));
	// 获取性能域里第一个 CPU 的额定算力，以进一步获得整个性能域中所有CPU的额定算力，因为性能域里所有 CPU 
	// 基于相同的微处理器架构，额定算力也是一样的
	scale_cpu = arch_scale_cpu_capacity(NULL, cpu);
	// 获取该性能域里频率最高的表项（table中频率是升序）
	cs = &pd->table[pd->nr_cap_states - 1];
	// map_util_freq() 函数做一个映射，为 CPU 最大实际算力 max_util、CPU 额定算力以及 OPP 表里的最高频率
	// 建立一个映射关系，以换算 max_util 对应的频率（freq）是多少
	freq = map_util_freq(max_util, cs->frequency, scale_cpu);

	/*
	 * Find the lowest capacity state of the Energy Model above the
	 * requested frequency.
	 */
	// 在 OPP 表里，查找一个正好和刚才换算出来的 freq 相等或者稍微大一点的频率点，接下来就使用这个频率点来计
	// 算整个性能域的功耗
	for (i = 0; i < pd->nr_cap_states; i++) {
		cs = &pd->table[i];
		if (cs->frequency >= freq)
			break;
	}

	/*
	 * The capacity of a CPU in the domain at that capacity state (cs)
	 * can be computed as:
	 *
	 *             cs->freq * scale_cpu
	 *   cs->cap = --------------------                          (1)
	 *                 cpu_max_freq
	 *
	 * So, ignoring the costs of idle states (which are not available in
	 * the EM), the energy consumed by this CPU at that capacity state is
	 * estimated as:
	 *
	 *             cs->power * cpu_util
	 *   cpu_nrg = --------------------                          (2)
	 *                   cs->cap
	 *
	 * since 'cpu_util / cs->cap' represents its percentage of busy time.
	 *
	 *   NOTE: Although the result of this computation actually is in
	 *         units of power, it can be manipulated as an energy value
	 *         over a scheduling period, since it is assumed to be
	 *         constant during that interval.
	 *
	 * By injecting (1) in (2), 'cpu_nrg' can be re-expressed as a product
	 * of two terms:
	 *
	 *             cs->power * cpu_max_freq   cpu_util
	 *   cpu_nrg = ------------------------ * ---------          (3)
	 *                    cs->freq            scale_cpu
	 *
	 * The first term is static, and is stored in the em_cap_state struct
	 * as 'cs->cost'.
	 *
	 * Since all CPUs of the domain have the same micro-architecture, they
	 * share the same 'cs->cost', and the same CPU capacity. Hence, the
	 * total energy of the domain (which is the simple sum of the energy of
	 * all of its CPUs) can be factorized as:
	 *
	 *            cs->cost * \Sum cpu_util
	 *   pd_nrg = ------------------------                       (4)
	 *                  scale_cpu
	 */
	// 计算性能域的功耗
	return cs->cost * sum_util / scale_cpu;
}

/**
 * em_pd_nr_cap_states() - Get the number of capacity states of a perf. domain
 * @pd		: performance domain for which this must be done
 *
 * Return: the number of capacity states in the performance domain table
 */
static inline int em_pd_nr_cap_states(struct em_perf_domain *pd)
{
	return pd->nr_cap_states;
}

#else
struct em_perf_domain {};
struct em_data_callback {};
#define EM_DATA_CB(_active_power_cb) { }

static inline int em_register_perf_domain(cpumask_t *span,
			unsigned int nr_states, struct em_data_callback *cb)
{
	return -EINVAL;
}
static inline struct em_perf_domain *em_cpu_get(int cpu)
{
	return NULL;
}
static inline unsigned long em_pd_energy(struct em_perf_domain *pd,
			unsigned long max_util, unsigned long sum_util)
{
	return 0;
}
static inline int em_pd_nr_cap_states(struct em_perf_domain *pd)
{
	return 0;
}
#endif

#endif
