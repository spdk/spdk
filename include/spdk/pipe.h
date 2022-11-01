/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2019 Intel Corporation. All rights reserved.
 */

/** \file
 * A pipe that is intended for buffering data between a source, such as
 * a socket, and a sink, such as a parser, or vice versa. Any time data
 * is received in units that differ from the the units it is consumed
 * in may benefit from using a pipe.
 *
 * The pipe is not thread safe. Only a single thread can act as both
 * the producer (called the writer) and the consumer (called the reader).
 */

#ifndef SPDK_PIPE_H
#define SPDK_PIPE_H

#include "spdk/stdinc.h"

struct spdk_pipe;

/**
 * Construct a pipe around the given memory buffer. The pipe treats the memory
 * buffer as a circular ring of bytes.
 *
 * The available size for writing will be one less byte than provided. A single
 * byte must be reserved to distinguish queue full from queue empty conditions.
 *
 * \param buf The data buffer that backs this pipe.
 * \param sz The size of the data buffer.
 *
 * \return spdk_pipe. The new pipe.
 */
struct spdk_pipe *spdk_pipe_create(void *buf, uint32_t sz);

/**
 * Destroys the pipe. This does not release the buffer, but does
 * make it safe for the user to release the buffer.
 *
 * \param pipe The pipe to operate on.
 */
void spdk_pipe_destroy(struct spdk_pipe *pipe);

/**
 * Acquire memory from the pipe for writing.
 *
 * This function will acquire up to sz bytes from the pipe to be used for
 * writing. It may return fewer total bytes.
 *
 * The memory is only marked as consumed upon a call to spdk_pipe_writer_advance().
 * Multiple calls to this function without calling advance return the same region
 * of memory.
 *
 * \param pipe The pipe to operate on.
 * \param sz The size requested.
 * \param iovs A two element iovec array that will be populated with the requested memory.
 *
 * \return The total bytes obtained. May be 0.
 */
int spdk_pipe_writer_get_buffer(struct spdk_pipe *pipe, uint32_t sz, struct iovec *iovs);

/**
 * Advance the write pointer by the given number of bytes
 *
 * The user can obtain memory from the pipe using spdk_pipe_writer_get_buffer(),
 * but only calling this function marks it as consumed. The user is not required
 * to advance the same number of bytes as was obtained from spdk_pipe_writer_get_buffer().
 * However, upon calling this function, the previous memory region is considered
 * invalid and the user must call spdk_pipe_writer_get_buffer() again to obtain
 * additional memory.
 *
 * The user cannot advance past the current read location.
 *
 * \param pipe The pipe to operate on.
 * \param count The number of bytes to advance.
 *
 * \return On error, a negated errno. On success, 0.
 */
int spdk_pipe_writer_advance(struct spdk_pipe *pipe, uint32_t count);

/**
 * Get the number of bytes available to read from the pipe.
 *
 * \param pipe The pipe to operate on.
 *
 * \return The number of bytes available for reading.
 */
uint32_t spdk_pipe_reader_bytes_available(struct spdk_pipe *pipe);

/**
 * Obtain previously written memory from the pipe for reading.
 *
 * This call populates the two element iovec provided with a region
 * of memory containing the next available data in the pipe. The size
 * will be up to sz bytes, but may be less.
 *
 * Calling this function does not mark the memory as consumed. Calling this function
 * twice without a call to spdk_pipe_reader_advance in between will return the same
 * region of memory.
 *
 * \param pipe The pipe to operate on.
 * \param sz The size requested.
 * \param iovs A two element iovec array that will be populated with the requested memory.
 *
 * \return On error, a negated errno. On success, the total number of bytes available.
 */
int spdk_pipe_reader_get_buffer(struct spdk_pipe *pipe, uint32_t sz, struct iovec *iovs);

/**
 * Mark memory as read, making it available for writing. The user is not required
 * to advance the same number of byte as was obtained by a previous call to
 * spdk_pipe_reader_get_buffer().
 *
 * \param pipe The pipe to operate on.
 * \param count The number of bytes to advance.
 *
 * \return On error, a negated errno. On success, 0.
 */
int spdk_pipe_reader_advance(struct spdk_pipe *pipe, uint32_t count);

#endif
