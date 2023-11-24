/*
 * Copyright (c) 2008 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * Distributed under the terms of the GNU GPL, version 2
 *
 * Please see kernel/locking/semaphore.c for documentation of these functions
 */
#ifndef __LINUX_SEMAPHORE_H
#define __LINUX_SEMAPHORE_H

#include <linux/list.h>
#include <linux/spinlock.h>

/* Please don't access any members of this structure directly */
struct semaphore {
	// lock 是自旋锁变量，用于保护 semaphore 数据结构里的 count 和 wait_list 变量
	raw_spinlock_t		lock;
	// 用于表示允许进入临界区的内核执行路径个数
	unsigned int		count;
	// 用于管理所有该信号量上睡眠的进程，没有成功获取锁的进程会在这个链表上睡眠
	struct list_head	wait_list;
};

#define __SEMAPHORE_INITIALIZER(name, n)				\
{									\
	.lock		= __RAW_SPIN_LOCK_UNLOCKED((name).lock),	\
	.count		= n,						\
	.wait_list	= LIST_HEAD_INIT((name).wait_list),		\
}

#define DEFINE_SEMAPHORE(name)	\
	struct semaphore name = __SEMAPHORE_INITIALIZER(name, 1)

static inline void sema_init(struct semaphore *sem, int val)
{
	static struct lock_class_key __key;
	// __SEMAPHORE_INITIALIZER 宏会完成对 semaphore 数据结构的填充，val 值通常设为 1
	*sem = (struct semaphore) __SEMAPHORE_INITIALIZER(*sem, val);
	lockdep_init_map(&sem->lock.dep_map, "semaphore->lock", &__key, 0);
}

// 在争用信号量失败时，进入不可中断的睡眠状态
extern void down(struct semaphore *sem);
// 在争用信号量失败时，进入可中断的睡眠状态
extern int __must_check down_interruptible(struct semaphore *sem);
extern int __must_check down_killable(struct semaphore *sem);
// 返回 0 表示成功获取锁，返回 1 表示获取锁失败
extern int __must_check down_trylock(struct semaphore *sem);
extern int __must_check down_timeout(struct semaphore *sem, long jiffies);
extern void up(struct semaphore *sem);

#endif /* __LINUX_SEMAPHORE_H */
