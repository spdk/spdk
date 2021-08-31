/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
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

#include "spdk/stdinc.h"
#include "spdk/log.h"
#include "spdk/trace_parser.h"
#include "spdk/util.h"

#include <exception>
#include <new>

struct spdk_trace_parser {
	spdk_trace_parser(const spdk_trace_parser_opts *opts);
	~spdk_trace_parser();
	spdk_trace_parser(const spdk_trace_parser &) = delete;
	spdk_trace_parser &operator=(const spdk_trace_parser &) = delete;
	const spdk_trace_flags *flags() const { return &_histories->flags; }
private:
	bool init(const spdk_trace_parser_opts *opts);
	void cleanup();

	spdk_trace_histories	*_histories;
	size_t			_map_size;
	int			_fd;
};

bool
spdk_trace_parser::init(const spdk_trace_parser_opts *opts)
{
	struct stat st;
	int rc;

	switch (opts->mode) {
	case SPDK_TRACE_PARSER_MODE_FILE:
		_fd = open(opts->filename, O_RDONLY);
		break;
	case SPDK_TRACE_PARSER_MODE_SHM:
		_fd = shm_open(opts->filename, O_RDONLY, 0600);
		break;
	default:
		SPDK_ERRLOG("Invalid mode: %d\n", opts->mode);
		return false;
	}

	if (_fd < 0) {
		SPDK_ERRLOG("Could not open trace file: %s (%d)\n", opts->filename, errno);
		return false;
	}

	rc = fstat(_fd, &st);
	if (rc < 0) {
		SPDK_ERRLOG("Could not get size of trace file: %s\n", opts->filename);
		return false;
	}

	if ((size_t)st.st_size < sizeof(*_histories)) {
		SPDK_ERRLOG("Invalid trace file: %s\n", opts->filename);
		return false;
	}

	/* Map the header of trace file */
	_map_size = sizeof(*_histories);
	_histories = static_cast<spdk_trace_histories *>(mmap(NULL, _map_size, PROT_READ,
			MAP_SHARED, _fd, 0));
	if (_histories == MAP_FAILED) {
		SPDK_ERRLOG("Could not mmap trace file: %s\n", opts->filename);
		_histories = NULL;
		return false;
	}

	/* Remap the entire trace file */
	_map_size = spdk_get_trace_histories_size(_histories);
	munmap(_histories, sizeof(*_histories));
	if ((size_t)st.st_size < _map_size) {
		SPDK_ERRLOG("Trace file %s is not valid\n", opts->filename);
		_histories = NULL;
		return false;
	}
	_histories = static_cast<spdk_trace_histories *>(mmap(NULL, _map_size, PROT_READ,
			MAP_SHARED, _fd, 0));
	if (_histories == MAP_FAILED) {
		SPDK_ERRLOG("Could not mmap trace file: %s\n", opts->filename);
		_histories = NULL;
		return false;
	}

	return true;
}

void
spdk_trace_parser::cleanup()
{
	if (_histories != NULL) {
		munmap(_histories, _map_size);
	}

	if (_fd > 0) {
		close(_fd);
	}
}

spdk_trace_parser::spdk_trace_parser(const spdk_trace_parser_opts *opts) :
	_histories(NULL),
	_map_size(0),
	_fd(-1)
{
	if (!init(opts)) {
		cleanup();
		throw std::exception();
	}
}

spdk_trace_parser::~spdk_trace_parser()
{
	cleanup();
}

struct spdk_trace_parser *
spdk_trace_parser_init(const struct spdk_trace_parser_opts *opts)
{
	try {
		return new spdk_trace_parser(opts);
	} catch (...) {
		return NULL;
	}
}

void
spdk_trace_parser_cleanup(struct spdk_trace_parser *parser)
{
	delete parser;
}

const struct spdk_trace_flags *
spdk_trace_parser_get_flags(const struct spdk_trace_parser *parser)
{
	return parser->flags();
}
