/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 Samsung Electonrics Co., Ltd.
 *   All rights reserved.
 */

/** \file
 * Network related helper functions
 */

#ifndef SPDK_NET_H
#define SPDK_NET_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Gets the name of the network interface for the given IP address.
 *
 * \param ip IP address to find the interface name for
 * \param ifc string output parameter for the interface name
 * \param len length of the ifc parameter in bytes
 *
 * \return 0 if successful, the interface name will be copied to the ifc parameter
 *         -ENODEV if an interface name could not be identified
 *         -ENOMEM the provided ifc string was too small
 */
int spdk_net_get_interface_name(const char *ip, char *ifc, size_t len);

/**
 * Gets the address string for a given struct sockaddr
 *
 * \param sa sockaddr to get the address string for
 * \param addr string to put the address
 * \param len length of the the addr parameter
 *
 * \return 0 if successful, negative -errno otherwise
 */
int spdk_net_get_address_string(struct sockaddr *sa, char *addr, size_t len);

/**
 * Checks if the given fd is a loopback interface or not.
 *
 * \param fd file descriptor to check
 *
 * \return true if the fd is loopback, false if not
 */
bool spdk_net_is_loopback(int fd);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
