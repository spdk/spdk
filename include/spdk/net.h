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
 * \return 0 on success, negative errno otherwise
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

/*
 * Get local and peer addresses of the given fd.
 *
 * \param fd file descriptor to get address.
 * \param laddr A pointer (may be NULL) to the buffer to hold the local address.
 * \param llen Length of the buffer 'laddr'.
 * \param lport A pointer (may be NULL) to the buffer to hold the local port info.
 * \param paddr A pointer (may be NULL) to the buffer to hold the peer address.
 * \param plen Length of the buffer 'paddr'.
 * \param pport A pointer (may be NULL) to the buffer to hold the peer port info.
 *
 * \return 0 on success, negative errno value on failure.
 */
int spdk_net_getaddr(int fd, char *laddr, int llen, uint16_t *lport,
		     char *paddr, int plen, uint16_t *pport);

/**
 * Compare two IP addresses to check if they are equal, and store the comparison
 * result in *cmp.
 *
 * The comparison result follows the same rule as strcmp, i.e. 0 if addr1 == addr2,
 * less than 0 if addr1 < addr2, and greater than 0 if addr1 > addr2.
 *
 * Note that the result is only valid when this function returns 0. Otherwise, the
 * content of *cmp will not be touched.
 *
 * \param adrfam Address family of the IP addresses, can be AF_INET or AF_INET6.
 * \param addr1 First IP address.
 * \param addr2 Second IP address.
 * \param cmp A pointer to the variable to store the result of the comparison.
 *
 * \return 0 on success, and *cmp contains the comparison result, -EAFNOSUPPORT if
 *         adrfam is not supported, or -EINVAL if the addresses are invalid.
 */
int spdk_net_compare_address(int adrfam, const char *addr1, const char *addr2, int *cmp);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NET_H */
