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
#ifndef FTL_DF_H
#define FTL_DF_H

#include "spdk/stdinc.h"

/* Durable format (df) object is an offset */
typedef uintptr_t ftl_df_obj_id;

#define FTL_DF_OBJ_ID_INVALID   ((ftl_df_obj_id)-1)

/**
 * @brief   Convert df object ptr to df object id
 *
 * @param   base        allocation base address
 * @param   df_obj_ptr  df object ptr
 *
 * @return  df object id
 */
static inline ftl_df_obj_id
ftl_df_get_obj_id(void *base, void *df_obj_ptr)
{
	assert(base <= df_obj_ptr);
	return ((char *)df_obj_ptr - (char *)base);
}

/**
 * @brief   Convert df object id to df object ptr
 *
 * @param   base        allocation base address
 * @param   df_obj_id   df object id
 *
 * @return  df object ptr
 */
static inline void *
ftl_df_get_obj_ptr(void *base, ftl_df_obj_id df_obj_id)
{
	assert(df_obj_id != FTL_DF_OBJ_ID_INVALID);
	return ((char *)base + df_obj_id);
}

#endif /* FTL_DF_H */
