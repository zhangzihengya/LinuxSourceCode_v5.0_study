/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CDEV_H
#define _LINUX_CDEV_H

#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>
#include <linux/device.h>

struct file_operations;
struct inode;
struct module;

struct cdev {
	struct kobject kobj;				// 用于Linux设备驱动模型
	struct module *owner;				// 字符设备驱动所在的内核模块对象指针
	const struct file_operations *ops;	// 字符设备的操作函数
	struct list_head list;				// 用来将字符设备串成一个链表
	dev_t dev;							// 字符设备的设备号，由主设备号和次设备号组成,高12位是主设备号，低20位是次设备号
	unsigned int count;					// 同属某个主设备号的次设备号的个数
} __randomize_layout;

void cdev_init(struct cdev *, const struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_set_parent(struct cdev *p, struct kobject *kobj);
int cdev_device_add(struct cdev *cdev, struct device *dev);
void cdev_device_del(struct cdev *cdev, struct device *dev);

void cdev_del(struct cdev *);

void cd_forget(struct inode *);

#endif
