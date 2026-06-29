/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 SPDK Hot Upgrade Contributors.
 *   All rights reserved.
 */

/**
 * \file
 * SPDK Hot Upgrade public API
 */

#ifndef SPDK_HOT_UPGRADE_H
#define SPDK_HOT_UPGRADE_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hot upgrade process state machine.
 */
enum spdk_hot_upgrade_state {
	SPDK_HU_IDLE = 0,
	SPDK_HU_SECONDARY_PRE_INIT,
	SPDK_HU_SECONDARY_PRE_INIT_DONE,
	SPDK_HU_PRIMARY_DRAINING,
	SPDK_HU_PRIMARY_SUSPENDED,
	SPDK_HU_SECONDARY_TAKEOVER,
	SPDK_HU_COMPLETE,
	SPDK_HU_FAILED,
};

/**
 * Set the hot upgrade state.
 *
 * \param state New state to transition to.
 */
void spdk_hot_upgrade_set_state(enum spdk_hot_upgrade_state state);

/**
 * Get the current hot upgrade state.
 *
 * \return Current state.
 */
enum spdk_hot_upgrade_state spdk_hot_upgrade_get_state(void);

/**
 * Check if a state transition is valid.
 *
 * \param from Source state.
 * \param to Destination state.
 * \return true if the transition is allowed.
 */
bool spdk_hot_upgrade_state_transition_valid(enum spdk_hot_upgrade_state from,
					     enum spdk_hot_upgrade_state to);

/**
 * Convert a state enum value to a human-readable string.
 *
 * \param state State to convert.
 * \return String representation of the state.
 */
const char *spdk_hot_upgrade_state_str(enum spdk_hot_upgrade_state state);

/**
 * Initialize the hot upgrade subsystem.
 * Must be called during SPDK app startup.
 *
 * \return 0 on success, negative errno on failure.
 */
int spdk_hot_upgrade_init(void);

/**
 * Check the hot upgrade subsystem is initialized.
 *
 * \return true if the hot upgrade subsystem is initialized.
 */
bool spdk_hot_upgrade_is_initialized(void);

/**
 * Check if SIGUSR1 signal has been received (for primary_resume).
 *
 * \return true if SIGUSR1 was received.
 */
bool spdk_hot_upgrade_sigusr1_received(void);

/**
 * Clear the SIGUSR1 received flag.
 */
void spdk_hot_upgrade_clear_sigusr1(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_HOT_UPGRADE_H */