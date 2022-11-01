/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_WRITER_H
#define FTL_WRITER_H

#include "spdk/stdinc.h"
#include "spdk/queue.h"

#include "ftl_io.h"

struct ftl_writer {
	struct spdk_ftl_dev *dev;

	TAILQ_HEAD(, ftl_rq) rq_queue;

	/* Band currently being written to */
	struct ftl_band	*band;

	/* Number of bands associated with writer */
	uint64_t num_bands;

	/* Band next being written to */
	struct ftl_band *next_band;

	/* List of full bands */
	TAILQ_HEAD(, ftl_band) full_bands;

	/* FTL band limit which blocks writes */
	int limit;

	/* Flag indicating halt has been requested */
	bool halt;

	/* Which type of band the writer uses */
	enum ftl_band_type writer_type;

	uint64_t last_seq_id;
};

bool ftl_writer_is_halted(struct ftl_writer *writer);

void ftl_writer_init(struct spdk_ftl_dev *dev, struct ftl_writer *writer,
		     uint64_t limit, enum ftl_band_type type);

void ftl_writer_run(struct ftl_writer *writer);

void ftl_writer_band_state_change(struct ftl_band *band);

static inline void
ftl_writer_halt(struct ftl_writer *writer)
{
	writer->halt = true;
}

static inline void
ftl_writer_resume(struct ftl_writer *writer)
{
	writer->halt = false;
}

bool ftl_writer_is_halted(struct ftl_writer *writer);

void ftl_writer_run(struct ftl_writer *writer);

static inline void
ftl_writer_queue_rq(struct ftl_writer *writer, struct ftl_rq *rq)
{
	TAILQ_INSERT_TAIL(&writer->rq_queue, rq, qentry);
}

/**
 * @brief Returns free space in currently processing band
 */
uint64_t ftl_writer_get_free_blocks(struct ftl_writer *writer);

#endif  /* FTL_WRITER_H */
