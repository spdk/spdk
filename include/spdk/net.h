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

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
