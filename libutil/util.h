/*
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 */

#ifndef UTIL_H
#define UTIL_H

#include <syslog.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({ \
		typeof(((type *)0)->member)*__mptr = (ptr); \
		(type *)((char *)__mptr - offsetof(type, member)); })
#endif

#ifndef MIN
#define MIN(a, b) \
	(__extension__ ({ \
		typeof(a) _a = (a); \
		typeof(b) _b = (b); \
		_a < _b ? _a : _b; \
	}))
#endif

#ifndef MAX
#define MAX(a, b) \
	(__extension__ ({ \
		typeof(a) _a = (a); \
		typeof(b) _b = (b); \
		_a > _b ? _a : _b; \
	}))
#endif

#ifndef BITS_PER_LONG
#define BITS_PER_LONG 64
#endif

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define BIT_ULL(nr)             (1ULL << (nr))

#define ALIGN_FLOOR(val, align) \
	((typeof(val))((val) & (~((typeof(val))((align) - 1)))))

#define ALIGN_CEIL(val, align) \
	ALIGN_FLOOR(((val) + ((typeof(val)) (align) - 1)), align)

#define	DIM(a) \
	(sizeof(a) / sizeof((a)[0]))

#define ONES32(size) \
	((size) ? (0xffffffff >> (32 - (size))) : 0xffffffff)

#define ROUND_DOWN_BITS(source, num_bits) \
	((source >> num_bits) << num_bits)

#define ROUND_UP_BITS(source, num_bits) \
	(ROUND_DOWN_BITS((source + ((1 << num_bits) - 1)), num_bits))

#define DIV_ROUND_UP_BITS(source, num_bits) \
	(ROUND_UP_BITS(source, num_bits) >> num_bits)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define log_error(M, ...) \
	syslog(LOG_ERR, "[ERROR] %s:%d:%s: " M "\n", \
	       __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define log_info(M, ...) \
	syslog(LOG_INFO,  "[INFO]  %s:%d:%s: " M "\n", \
	       __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define log_debug(M, ...) \
	syslog(LOG_DEBUG, "[DEBUG] %s:%d:%s: " M "\n", \
	       __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define log_warn(M, ...) \
	syslog(LOG_WARNING, "[WARNING] %s:%d:%s: " M "\n", \
	       __FILE__, __LINE__, __func__, ##__VA_ARGS__)

#define err_if(A, M, ...) \
	if((A)) {log_error(M, ##__VA_ARGS__); goto free_exit;}

#ifdef __cplusplus
}
#endif

#endif
