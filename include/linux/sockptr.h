/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2020 Christoph Hellwig.
 *
 * Support for "universal" pointers that can point to either kernel or userspace
 * memory.
 */
#ifndef _LINUX_SOCKPTR_H
#define _LINUX_SOCKPTR_H

#include <linux/compiler.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#ifdef CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE
typedef union {
	void		*kernel;
	void __user	*user;
} sockptr_t;

static inline bool sockptr_is_kernel(sockptr_t sockptr)
{
	return (unsigned long)sockptr.kernel >= TASK_SIZE;
}

static inline sockptr_t KERNEL_SOCKPTR(void *p)
{
	return (sockptr_t) { .kernel = p };
}

static inline int __must_check init_user_sockptr(sockptr_t *sp, void __user *p)
{
	if ((unsigned long)p >= TASK_SIZE)
		return -EFAULT;
	sp->user = p;
	return 0;
}
#else /* CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE */
typedef struct {
	union {
		void		*kernel;
		void __user	*user;
	};
	bool		is_kernel : 1;
} sockptr_t;

static inline bool sockptr_is_kernel(sockptr_t sockptr)
{
	return sockptr.is_kernel;
}

static inline sockptr_t KERNEL_SOCKPTR(void *p)
{
	return (sockptr_t) { .kernel = p, .is_kernel = true };
}

static inline int __must_check init_user_sockptr(sockptr_t *sp, void __user *p)
{
	sp->user = p;
	sp->is_kernel = false;
	return 0;
}
#endif /* CONFIG_ARCH_HAS_NON_OVERLAPPING_ADDRESS_SPACE */

static inline bool sockptr_is_null(sockptr_t sockptr)
{
	return !sockptr.user && !sockptr.kernel;
}

static inline int copy_from_sockptr(void *dst, sockptr_t src, size_t size)
{
	if (!sockptr_is_kernel(src))
		return copy_from_user(dst, src.user, size);
	memcpy(dst, src.kernel, size);
	return 0;
}

static inline int copy_to_sockptr(sockptr_t dst, const void *src, size_t size)
{
	if (!sockptr_is_kernel(dst))
		return copy_to_user(dst.user, src, size);
	memcpy(dst.kernel, src, size);
	return 0;
}

static inline void *memdup_sockptr(sockptr_t src, size_t len)
{
	void *p = kmalloc_track_caller(len, GFP_USER | __GFP_NOWARN);

	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_sockptr(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	return p;
}

static inline void *memdup_sockptr_nul(sockptr_t src, size_t len)
{
	char *p = kmalloc_track_caller(len + 1, GFP_KERNEL);

	if (!p)
		return ERR_PTR(-ENOMEM);
	if (copy_from_sockptr(p, src, len)) {
		kfree(p);
		return ERR_PTR(-EFAULT);
	}
	p[len] = '\0';
	return p;
}

static inline void sockptr_advance(sockptr_t sockptr, size_t len)
{
	if (sockptr_is_kernel(sockptr))
		sockptr.kernel += len;
	else
		sockptr.user += len;
}

static inline long strncpy_from_sockptr(char *dst, sockptr_t src, size_t count)
{
	if (sockptr_is_kernel(src)) {
		size_t len = min(strnlen(src.kernel, count - 1) + 1, count);

		memcpy(dst, src.kernel, len);
		return len;
	}
	return strncpy_from_user(dst, src.user, count);
}

#endif /* _LINUX_SOCKPTR_H */
