#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2021 Intel Corporation.
#  All rights reserved.

try:
    # version.py is generated during the build, so ignore it if it doesn't exist
    from .version import __version__  # type: ignore[import-untyped]
except ModuleNotFoundError:
    __version__ = '0.0'
