/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2008-2012 Daisuke Aoyama <aoyama@peach.ne.jp>.
 *   Copyright (C) 2016 Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * Configuration file parser
 */

#ifndef SPDK_CONF_H
#define SPDK_CONF_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_conf_value;
struct spdk_conf_item;
struct spdk_conf_section;
struct spdk_conf;

/**
 * Allocate a configuration struct used for the initialization of SPDK app.
 *
 * \return a pointer to the allocated configuration struct.
 */
struct spdk_conf *spdk_conf_allocate(void);

/**
 * Free the configuration struct.
 *
 * \param cp Configuration struct to free.
 */
void spdk_conf_free(struct spdk_conf *cp);

/**
 * Read configuration file for spdk_conf struct.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 * \param file File to read that is created by user to configure SPDK app.
 *
 * \return 0 on success, -1 on failure.
 */
int spdk_conf_read(struct spdk_conf *cp, const char *file);

/**
 * Find the specified section of the configuration.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 * \param name Name of section to find.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
struct spdk_conf_section *spdk_conf_find_section(struct spdk_conf *cp, const char *name);

/**
 * Get the first section of the configuration.
 *
 * \param cp Configuration struct used for the initialization of SPDK app.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
struct spdk_conf_section *spdk_conf_first_section(struct spdk_conf *cp);

/**
 * Get the next section of the configuration.
 *
 * \param sp The current section of the configuration.
 *
 * \return a pointer to the requested section on success or NULL otherwise.
 */
struct spdk_conf_section *spdk_conf_next_section(struct spdk_conf_section *sp);

/**
 * Match prefix of the name of section.
 *
 * \param sp The section of the configuration.
 * \param name_prefix Prefix name to match.
 *
 * \return true on success, false on failure.
 */
bool spdk_conf_section_match_prefix(const struct spdk_conf_section *sp, const char *name_prefix);

/**
 * Get the name of the section.
 *
 * \param sp The section of the configuration.
 *
 * \return the name of the section.
 */
const char *spdk_conf_section_get_name(const struct spdk_conf_section *sp);

/**
 * Get the number of the section.
 *
 * \param sp The section of the configuration.
 *
 * \return the number of the section.
 */
int spdk_conf_section_get_num(const struct spdk_conf_section *sp);

/**
 * Get the value of the item with name 'key' in the section.
 *
 * If key appears multiple times, idx1 will control which version to retrieve.
 * Indices will start from the top of the configuration file at 0 and increment
 * by one for each new appearance. If the configuration key contains multiple
 * whitespace delimited values, idx2 controls which value is returned. The index
 * begins at 0.
 *
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param idx1 The index into the item list for the key.
 * \param idx2 The index into the value list for the item.
 *
 * \return the requested value on success or NULL otherwise.
 */
char *spdk_conf_section_get_nmval(struct spdk_conf_section *sp, const char *key,
				  int idx1, int idx2);

/**
 * Get the first value of the item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param idx The index into the value list for the item.
 *
 * \return the requested value on success or NULL otherwise.
 */
char *spdk_conf_section_get_nval(struct spdk_conf_section *sp, const char *key, int idx);

/**
 * Get the first value of the first item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 *
 * \return the requested value on success or NULL otherwise.
 */
char *spdk_conf_section_get_val(struct spdk_conf_section *sp, const char *key);

/**
 * Get the first value of the first item with name 'key' in the section.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 *
 * \return the requested value on success or NULL otherwise.
 */
int spdk_conf_section_get_intval(struct spdk_conf_section *sp, const char *key);

/**
 * Get the bool value of the item with name 'key' in the section.
 *
 * This is used to check whether the service is enabled.
 *
 * \param sp The section of the configuration.
 * \param key Name of item.
 * \param default_val Default value.
 *
 * \return true if matching 'Yes/Y/True', false if matching 'No/N/False', default value otherwise.
 */
bool spdk_conf_section_get_boolval(struct spdk_conf_section *sp, const char *key, bool default_val);

/**
 * Set the configuration as the default.
 *
 * \param cp Configuration to set.
 */
void spdk_conf_set_as_default(struct spdk_conf *cp);

/**
 * Disable sections merging during 'spdk_conf_read()'
 *
 * \param cp Configuration to be read
 */
void spdk_conf_disable_sections_merge(struct spdk_conf *cp);

#ifdef __cplusplus
}
#endif

#endif
