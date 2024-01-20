/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MIGRATE_MODE_H_INCLUDED
#define MIGRATE_MODE_H_INCLUDED
/*
 * MIGRATE_ASYNC means never block
 * MIGRATE_SYNC_LIGHT in the current implementation means to allow blocking
 *	on most operations but not ->writepage as the potential stall time
 *	is too significant
 * MIGRATE_SYNC will block when migrating pages
 * MIGRATE_SYNC_NO_COPY will block when migrating pages but will not copy pages
 *	with the CPU. Instead, page copy happens outside the migratepage()
 *	callback and is likely using a DMA engine. See migrate_vma() and HMM
 *	(mm/hmm.c) for users of this mode.
 */
enum migrate_mode {
	// 异步模式
	// 在判断内存规整是否完成时，若可以从其它迁移类型中挪用空闲页块，那么也算完成任务
	// 在分离页面时，若发现大量的临时分离页面（即分离的页面数量大于 LRU 页面数量的一半），也不会临时暂停扫描
	// 当进程需要调度时，退出内存规整
	MIGRATE_ASYNC,
	// 同步模式，允许调用者被阻塞
	MIGRATE_SYNC_LIGHT,
	// 同步模式，在页面迁移时会被阻塞
	MIGRATE_SYNC,
	// 类似于同步模式，但是在迁移页面时 CPU 不会复制页面的内容，而是由 DMA 引擎来复制
	MIGRATE_SYNC_NO_COPY,
};

#endif		/* MIGRATE_MODE_H_INCLUDED */
