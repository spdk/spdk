#   SPDX-License-Identifier: BSD-3-Clause
#   Copyright (C) 2022 Intel Corporation.
#   All rights reserved.
#


def compressdev_scan_accel_module(client, pmd):
    """Scan and enable compressdev module and set pmd option.

    Args:
        pmd: 0 = auto-select, 1 = QAT, 2 = mlx5_pci
    """
    params = {'pmd': pmd}

    return client.call('compressdev_scan_accel_module', params)
