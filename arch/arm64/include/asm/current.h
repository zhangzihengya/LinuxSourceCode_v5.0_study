/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_CURRENT_H
#define __ASM_CURRENT_H

#include <linux/compiler.h>

#ifndef __ASSEMBLY__

struct task_struct;

/*
 * We don't use read_sysreg() as we want the compiler to cache the value where
 * possible.
 */
// 在内核态，ARM64 处理器运行在 EL1 下，sp_el0 寄存器在 EL1 上下文中没有使用。
// 利用 sp_el0 寄存器来存放 task_struct 数据结构的地址是一种简洁有效的办法
static __always_inline struct task_struct *get_current(void)
{
	unsigned long sp_el0;

	// 使用汇编指令从系统寄存器中读取 EL0 模式的堆栈指针的值，并将其存储在一个无符号长整型变量 sp_el0 中
	// "mrs %0, sp_el0" 是内联汇编指令，"mrs" 表示读取系统寄存器的值
	// "%0" 表示占位符，表示输出操作数，与下面的": "=r" (sp_el0)" 相对应
	// "sp_el0" 是一个特殊的系统寄存器，包含 EL0 模式的堆栈指针
	asm ("mrs %0, sp_el0" : "=r" (sp_el0));

	return (struct task_struct *)sp_el0;
}

#define current get_current()

#endif /* __ASSEMBLY__ */

#endif /* __ASM_CURRENT_H */

