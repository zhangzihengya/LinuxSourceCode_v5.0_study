/*
 * Based on arch/arm/mm/context.c
 *
 * Copyright (C) 2002-2003 Deep Blue Solutions Ltd, all rights reserved.
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/bitops.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>

#include <asm/cpufeature.h>
#include <asm/mmu_context.h>
#include <asm/smp.h>
#include <asm/tlbflush.h>

static u32 asid_bits;
static DEFINE_RAW_SPINLOCK(cpu_asid_lock);

static atomic64_t asid_generation;
static unsigned long *asid_map;

static DEFINE_PER_CPU(atomic64_t, active_asids);
static DEFINE_PER_CPU(u64, reserved_asids);
static cpumask_t tlb_flush_pending;

#define ASID_MASK		(~GENMASK(asid_bits - 1, 0))
#define ASID_FIRST_VERSION	(1UL << asid_bits)

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
#define NUM_USER_ASIDS		(ASID_FIRST_VERSION >> 1)
#define asid2idx(asid)		(((asid) & ~ASID_MASK) >> 1)
#define idx2asid(idx)		(((idx) << 1) & ~ASID_MASK)
#else
#define NUM_USER_ASIDS		(ASID_FIRST_VERSION)
#define asid2idx(asid)		((asid) & ~ASID_MASK)
#define idx2asid(idx)		asid2idx(idx)
#endif

/* Get the ASIDBits supported by the current CPU */
static u32 get_cpu_asid_bits(void)
{
	u32 asid;
	int fld = cpuid_feature_extract_unsigned_field(read_cpuid(ID_AA64MMFR0_EL1),
						ID_AA64MMFR0_ASID_SHIFT);

	switch (fld) {
	default:
		pr_warn("CPU%d: Unknown ASID size (%d); assuming 8-bit\n",
					smp_processor_id(),  fld);
		/* Fallthrough */
	case 0:
		asid = 8;
		break;
	case 2:
		asid = 16;
	}

	return asid;
}

/* Check if the current cpu's ASIDBits is compatible with asid_bits */
void verify_cpu_asid_bits(void)
{
	u32 asid = get_cpu_asid_bits();

	if (asid < asid_bits) {
		/*
		 * We cannot decrease the ASID size at runtime, so panic if we support
		 * fewer ASID bits than the boot CPU.
		 */
		pr_crit("CPU%d: smaller ASID size(%u) than boot CPU (%u)\n",
				smp_processor_id(), asid, asid_bits);
		cpu_panic_kernel();
	}
}

static void flush_context(void)
{
	int i;
	u64 asid;

	/* Update the list of reserved ASIDs and the ASID bitmap. */
	bitmap_clear(asid_map, 0, NUM_USER_ASIDS);

	for_each_possible_cpu(i) {
		asid = atomic64_xchg_relaxed(&per_cpu(active_asids, i), 0);
		/*
		 * If this CPU has already been through a
		 * rollover, but hasn't run another task in
		 * the meantime, we must preserve its reserved
		 * ASID, as this is the only trace we have of
		 * the process it is still running.
		 */
		if (asid == 0)
			asid = per_cpu(reserved_asids, i);
		__set_bit(asid2idx(asid), asid_map);
		per_cpu(reserved_asids, i) = asid;
	}

	/*
	 * Queue a TLB invalidation for each CPU to perform on next
	 * context-switch
	 */
	cpumask_setall(&tlb_flush_pending);
}

static bool check_update_reserved_asid(u64 asid, u64 newasid)
{
	int cpu;
	bool hit = false;

	/*
	 * Iterate over the set of reserved ASIDs looking for a match.
	 * If we find one, then we can update our mm to use newasid
	 * (i.e. the same ASID in the current generation) but we can't
	 * exit the loop early, since we need to ensure that all copies
	 * of the old ASID are updated to reflect the mm. Failure to do
	 * so could result in us missing the reserved ASID in a future
	 * generation.
	 */
	for_each_possible_cpu(cpu) {
		if (per_cpu(reserved_asids, cpu) == asid) {
			hit = true;
			per_cpu(reserved_asids, cpu) = newasid;
		}
	}

	return hit;
}

static u64 new_context(struct mm_struct *mm)
{
	static u32 cur_idx = 1;
	// 获取当前进程的 ASID
	u64 asid = atomic64_read(&mm->context.id);
	// 获取当前系统的 ASID，这个值存储在全局原子变量 asid_generation 中 
	u64 generation = atomic64_read(&asid_generation);

	// 刚创建进程时，mm->context.id 值初始化为 0。如果这时 ASID 不为 0，说明该进程已经分配过 ASID。
	// 如果原来的 ASID 还有效，那么只需要再加上新的 generation 值即可组成一个新的软件 ASID
	if (asid != 0) {
		u64 newasid = generation | (asid & ~ASID_MASK);

		/*
		 * If our current ASID was active during a rollover, we
		 * can continue to use it and this was just a false alarm.
		 */
		// 判断当前的 ASID 是否有效
		if (check_update_reserved_asid(asid, newasid))
			return newasid;

		/*
		 * We had a valid ASID in a previous life, so try to re-use
		 * it if possible.
		 */
		if (!__test_and_set_bit(asid2idx(asid), asid_map))
			return newasid;
	}

	/*
	 * Allocate a free ASID. If we can't find one, take a note of the
	 * currently active ASIDs and mark the TLBs as requiring flushes.  We
	 * always count from ASID #2 (index 1), as we use ASID #0 when setting
	 * a reserved TTBR0 for the init_mm and we allocate ASIDs in even/odd
	 * pairs.
	 */
	// 如果之前的硬件 ASID 不能使用，那么从 asid_map 中查找第一个空闲的位，并将其作为这次的硬件 ASID
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, cur_idx);
	if (asid != NUM_USER_ASIDS)
		goto set_asid;

	/* We're out of ASIDs, so increment the global generation count */
	// 如果找不到一个空闲的位，说明发生了溢出，那么只能提升 generation 值，并调用 flush_context() 函数
	// 刷新所有 CPU 上的 TLB，同时把 asid_map 清零
	generation = atomic64_add_return_relaxed(ASID_FIRST_VERSION,
						 &asid_generation);
	flush_context();

	/* We have more ASIDs than CPUs, so this will always succeed */
	// 在 asid_map 中找到一个空闲的位，这次一定能成功，因为刚才已经把 asid_map 清零了
	asid = find_next_zero_bit(asid_map, NUM_USER_ASIDS, 1);

// 新生成一个 ASID
set_asid:
	__set_bit(asid, asid_map);
	cur_idx = asid;
	// 返回一个新的软件 ASID
	return idx2asid(asid) | generation;
}

void check_and_switch_context(struct mm_struct *mm, unsigned int cpu)
{
	unsigned long flags;
	u64 asid, old_active_asid;

	if (system_supports_cnp())
		cpu_set_reserved_ttbr0();

	// 通过原子操作读取软件的 ASID
	asid = atomic64_read(&mm->context.id);

	/*
	 * The memory ordering here is subtle.
	 * If our active_asids is non-zero and the ASID matches the current
	 * generation, then we update the active_asids entry with a relaxed
	 * cmpxchg. Racing with a concurrent rollover means that either:
	 *
	 * - We get a zero back from the cmpxchg and end up waiting on the
	 *   lock. Taking the lock synchronises with the rollover and so
	 *   we are forced to see the updated generation.
	 *
	 * - We get a valid ASID back from the cmpxchg, which means the
	 *   relaxed xchg in flush_context will treat us as reserved
	 *   because atomic RmWs are totally ordered for a given location.
	 */
	// 读取 Per-CPU 变量的 active_asids
	old_active_asid = atomic64_read(&per_cpu(active_asids, cpu));
	// 判断全局原子变量 asid_generation 存储的软件 generation 计数和进程内存描述符存储的软件 generation 计数是否相同
	// 另外还需要通过 atomic64_cmpxchg() 原子交换指令来设置新的 asid 到 Per-CPU 变量 active_asids 中
	if (old_active_asid &&
	    !((asid ^ atomic64_read(&asid_generation)) >> asid_bits) &&
	    atomic64_cmpxchg_relaxed(&per_cpu(active_asids, cpu),
				     old_active_asid, asid))
		goto switch_mm_fastpath;

	raw_spin_lock_irqsave(&cpu_asid_lock, flags);
	/* Check that our ASID belongs to the current generation. */
	// 重新做一次软件 generation 计数的比较，如果还不相同，说明至少发生了一次 ASID 硬件溢出，需要分配一个新的软件 ASID 计数
	// 并设置到 mm->context.id 中
	asid = atomic64_read(&mm->context.id);
	if ((asid ^ atomic64_read(&asid_generation)) >> asid_bits) {
		asid = new_context(mm);
		atomic64_set(&mm->context.id, asid);
	}

	// 硬件 ASID 发生溢出时，需要刷新本地的 TLB
	if (cpumask_test_and_clear_cpu(cpu, &tlb_flush_pending))
		local_flush_tlb_all();

	atomic64_set(&per_cpu(active_asids, cpu), asid);
	raw_spin_unlock_irqrestore(&cpu_asid_lock, flags);

// switch_mm_fastpath 标签表示换入进程的 ASID 依然属于同一个批次，也就是说还没有发生 ASID 硬件溢出
switch_mm_fastpath:

	arm64_apply_bp_hardening();

	/*
	 * Defer TTBR0_EL1 setting for user threads to uaccess_enable() when
	 * emulating PAN.
	 */
	if (!system_uses_ttbr0_pan())
		// 进行页表的切换
		cpu_switch_mm(mm->pgd, mm);
}

/* Errata workaround post TTBRx_EL1 update. */
asmlinkage void post_ttbr_update_workaround(void)
{
	asm(ALTERNATIVE("nop; nop; nop",
			"ic iallu; dsb nsh; isb",
			ARM64_WORKAROUND_CAVIUM_27456,
			CONFIG_CAVIUM_ERRATUM_27456));
}

static int asids_init(void)
{
	asid_bits = get_cpu_asid_bits();
	/*
	 * Expect allocation after rollover to fail if we don't have at least
	 * one more ASID than CPUs. ASID #0 is reserved for init_mm.
	 */
	WARN_ON(NUM_USER_ASIDS - 1 <= num_possible_cpus());
	atomic64_set(&asid_generation, ASID_FIRST_VERSION);
	asid_map = kcalloc(BITS_TO_LONGS(NUM_USER_ASIDS), sizeof(*asid_map),
			   GFP_KERNEL);
	if (!asid_map)
		panic("Failed to allocate bitmap for %lu ASIDs\n",
		      NUM_USER_ASIDS);

	pr_info("ASID allocator initialised with %lu entries\n", NUM_USER_ASIDS);
	return 0;
}
early_initcall(asids_init);
