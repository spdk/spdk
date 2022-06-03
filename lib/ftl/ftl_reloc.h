/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_RELOC_H
#define FTL_RELOC_H

#include "spdk/stdinc.h"
#include "spdk/ftl.h"

struct ftl_reloc;
struct ftl_band;

struct ftl_reloc	*ftl_reloc_init(struct spdk_ftl_dev *dev);
void			ftl_reloc_free(struct ftl_reloc *reloc);
void			ftl_reloc_add(struct ftl_reloc *reloc, struct ftl_band *band,
				      size_t offset, size_t num_blocks, int prio, bool is_defrag);
bool			ftl_reloc(struct ftl_reloc *reloc);
void			ftl_reloc_halt(struct ftl_reloc *reloc);
void			ftl_reloc_resume(struct ftl_reloc *reloc);
bool			ftl_reloc_is_halted(const struct ftl_reloc *reloc);
bool			ftl_reloc_is_defrag_active(const struct ftl_reloc *reloc);

#endif /* FTL_RELOC_H */
