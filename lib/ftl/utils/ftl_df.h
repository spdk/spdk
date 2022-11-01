/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2022 Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_DF_H
#define FTL_DF_H

#include "spdk/stdinc.h"

/* Durable format (df) object is an offset */
typedef uint64_t ftl_df_obj_id;

#define FTL_DF_OBJ_ID_INVALID ((ftl_df_obj_id)-1)

/**
 * @brief Convert df object ptr to df object id
 *
 * @param base		allocation base address
 * @param df_obj_ptr	df object ptr
 *
 * @return df object id
 */
static inline ftl_df_obj_id
ftl_df_get_obj_id(void *base, void *df_obj_ptr)
{
	assert(base <= df_obj_ptr);
	return ((char *)df_obj_ptr - (char *)base);
}

/**
 * @brief Convert df object id to df object ptr
 *
 * @param base		allocation base address
 * @param df_obj_id	df object id
 *
 * @return df object ptr
 */
static inline void *
ftl_df_get_obj_ptr(void *base, ftl_df_obj_id df_obj_id)
{
	assert(df_obj_id != FTL_DF_OBJ_ID_INVALID);
	return ((char *)base + df_obj_id);
}

#endif /* FTL_DF_H */
