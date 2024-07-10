/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_MOCK_H
#define SPDK_INTERNAL_MOCK_H

#include "spdk/queue.h"
#include "spdk/stdinc.h"

#define MOCK_STRUCT_INIT(...) \
	{ __VA_ARGS__ }

#define DEFINE_RETURN_MOCK(fn, ret) \
	DECLARE_MOCK_QUEUE(fn, ret);		\
	DEFINE_MOCK_QUEUE(fn, ret);		\
	bool ut_ ## fn ## _mocked = false;	\
	ret ut_ ## fn

#define DECLARE_MOCK_QUEUE(fn, ret) \
	struct ut_mqe_ ## fn {						\
		ret				val;			\
		TAILQ_ENTRY(ut_mqe_ ## fn)	link;			\
	};								\
	extern TAILQ_HEAD(ut_mqh_ ## fn, ut_mqe_ ## fn) ut_mqh_ ## fn;	\
	ret ut_mq_dequeue_ ## fn (void);					\

#define DEFINE_MOCK_QUEUE(fn, ret) \
	struct ut_mqh_ ## fn ut_mqh_ ## fn =				\
		TAILQ_HEAD_INITIALIZER(ut_mqh_ ## fn);			\
									\
	ret ut_mq_dequeue_ ## fn (void)					\
	{								\
		struct ut_mqe_ ## fn *mqe;				\
		ret val;						\
		mqe = TAILQ_FIRST(&ut_mqh_ ## fn);			\
		TAILQ_REMOVE(&ut_mqh_ ## fn, mqe, link);		\
		val = mqe->val;						\
		free(mqe);						\
		return val;						\
	}
/*
 * For controlling mocked function behavior, setting
 * and getting values from the stub, the _P macros are
 * for mocking functions that return pointer values.
 */
#define MOCK_SET(fn, val) \
	ut_ ## fn ## _mocked = true; \
	ut_ ## fn = val

/*
 * MOCK_ENQUEUE() can be used to specify multiple different return values for the same function.
 * Each consecutive call to a function will return a value specified by this macro (in a FIFO
 * fashion).  Once all such values are exhausted (or none has been specified), the value assigned by
 * MOCK_SET() will be returned.
 */
#define MOCK_ENQUEUE(fn, _val) do { \
		struct ut_mqe_ ## fn *mqe = calloc(1, sizeof(*mqe));	\
		mqe->val = _val;					\
		TAILQ_INSERT_TAIL(&ut_mqh_ ## fn, mqe, link);		\
	} while (0)

#define MOCK_GET(fn) \
	!TAILQ_EMPTY(&ut_mqh_ ## fn) ? ut_mq_dequeue_ ## fn () : ut_ ## fn

#define MOCK_CLEAR_QUEUE(fn) do { \
		struct ut_mqe_ ## fn *mqe;				\
		while ((mqe = TAILQ_FIRST(&ut_mqh_ ## fn))) {		\
			TAILQ_REMOVE(&ut_mqh_ ## fn, mqe, link);	\
			free(mqe);					\
		}							\
	} while (0)

#define MOCK_CLEAR(fn) do { \
		ut_ ## fn ## _mocked = false;	\
		MOCK_CLEAR_QUEUE(fn);		\
	} while (0)

#define MOCK_CLEAR_P(fn) do { \
		ut_ ## fn ## _mocked = false;	\
		ut_ ## fn = NULL;		\
		MOCK_CLEAR_QUEUE(fn);		\
	} while (0)

/* for proving to *certain* static analysis tools that we didn't reset the mock function. */
#define MOCK_CLEARED_ASSERT(fn) \
	SPDK_CU_ASSERT_FATAL(ut_ ## fn ## _mocked == false)

/* for declaring function prototypes for wrappers */
#define DECLARE_WRAPPER(fn, ret, args) \
	DECLARE_MOCK_QUEUE(fn, ret); \
	extern bool ut_ ## fn ## _mocked; \
	extern ret ut_ ## fn; \
	__attribute__((used)) ret __wrap_ ## fn args; ret __real_ ## fn args

/*
 * For defining the implementation of wrappers for syscalls.
 * Avoid nested macro calls to prevent macro expansion of fn.
 */
#define DEFINE_WRAPPER(fn, ret, dargs, pargs) \
	DEFINE_WRAPPER_MOCK(fn, ret); \
	ret __wrap_ ## fn dargs \
	{ \
		if (!ut_ ## fn ## _mocked) { \
			return __real_ ## fn pargs; \
		} else { \
			return ut_ ## fn; \
		} \
	}

#define DEFINE_WRAPPER_MOCK(fn, ret) \
	DEFINE_MOCK_QUEUE(fn, ret);		\
	bool ut_ ## fn ## _mocked = false;	\
	ret ut_ ## fn

/* DEFINE_STUB is for defining the implementation of stubs for SPDK funcs. */
#define DEFINE_STUB(fn, ret, dargs, val) \
	DECLARE_MOCK_QUEUE(fn, ret); \
	DEFINE_MOCK_QUEUE(fn, ret); \
	bool ut_ ## fn ## _mocked = true; \
	ret ut_ ## fn = val; \
	ret fn dargs; \
	ret fn dargs \
	{ \
		return MOCK_GET(fn); \
	}

/* DEFINE_STUB_V macro is for stubs that don't have a return value */
#define DEFINE_STUB_V(fn, dargs) \
	void fn dargs; \
	void fn dargs \
	{ \
	}

#define HANDLE_RETURN_MOCK(fn)  \
	if (!TAILQ_EMPTY(&ut_mqh_ ## fn)) {	\
		return ut_mq_dequeue_ ## fn ();	\
	}					\
	if (ut_ ## fn ## _mocked) {		\
		return ut_ ## fn;		\
	}					\


/* declare wrapper protos (alphabetically please) here */
DECLARE_WRAPPER(calloc, void *, (size_t nmemb, size_t size));

DECLARE_WRAPPER(pthread_mutex_init, int,
		(pthread_mutex_t *mtx, const pthread_mutexattr_t *attr));

DECLARE_WRAPPER(pthread_mutexattr_init, int,
		(pthread_mutexattr_t *attr));

DECLARE_WRAPPER(recvmsg, ssize_t, (int sockfd, struct msghdr *msg, int flags));

DECLARE_WRAPPER(sendmsg, ssize_t, (int sockfd, const struct msghdr *msg, int flags));

DECLARE_WRAPPER(writev, ssize_t, (int fd, const struct iovec *iov, int iovcnt));

/* unlink is done a bit differently. */
extern char *g_unlink_path;
extern void (*g_unlink_callback)(void);
/* If g_unlink_path is NULL, __wrap_unlink will return ENOENT.
 * If the __wrap_unlink() parameter does not match g_unlink_path, it will return ENOENT.
 * If g_unlink_path does match, and g_unlink_callback has been set, g_unlink_callback will
 * be called before returning 0.
 */
int __wrap_unlink(const char *path);

#endif /* SPDK_INTERNAL_MOCK_H */
