/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 * Trace parser library
 */

#ifndef SPDK_TRACE_PARSER_H
#define SPDK_TRACE_PARSER_H

#include "spdk/stdinc.h"
#include "spdk/trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Trace parser object used as a context for the parsing functions */
struct spdk_trace_parser;

enum spdk_trace_parser_mode {
	/** Regular file */
	SPDK_TRACE_PARSER_MODE_FILE,
	/** Shared memory */
	SPDK_TRACE_PARSER_MODE_SHM,
};

/** Describes trace file and options to use when parsing it */
struct spdk_trace_parser_opts {
	/** Either file name or shared memory name depending on mode */
	const char	*filename;
	/** Trace file type, either regular file or shared memory */
	int		mode;
	/** Logical core number to parse the traces from (or SPDK_TRACE_MAX_LCORE) for all cores */
	uint16_t	lcore;
};

/**
 * Initialize the parser using a specified trace file.  This results in parsing the traces, merging
 * entries from multiple cores together and sorting them by their tsc, so it can take a significant
 * amount of time to complete.
 *
 * \param opts Describes the trace file to parse.
 *
 * \return Parser object or NULL in case of any failures.
 */
struct spdk_trace_parser *spdk_trace_parser_init(const struct spdk_trace_parser_opts *opts);

/**
 * Free any resources tied to a parser object.
 *
 * \param parser Parser to clean up.
 */
void spdk_trace_parser_cleanup(struct spdk_trace_parser *parser);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_TRACE_PARSER_H */
