#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES.
#  All rights reserved.

def dpdk_cryptodev_scan_accel_module(client):
    """Enable dpdk_cryptodev accel module.
    """
    return client.call('dpdk_cryptodev_scan_accel_module')


def dpdk_cryptodev_set_driver(client, driver_name):
    """Set the DPDK cryptodev driver.

    Args:
        driver_name: The driver, can be one of crypto_aesni_mb, crypto_qat or mlx5_pci
    """
    params = {'driver_name': driver_name}

    return client.call('dpdk_cryptodev_set_driver', params)


def dpdk_cryptodev_get_driver(client):
    """Get the DPDK cryptodev driver.
    """
    return client.call('dpdk_cryptodev_get_driver')
