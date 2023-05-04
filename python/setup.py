#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (C) 2023 Intel Corporation.  All rights reserved.

from distutils.core import setup
from setuptools import find_packages
from spdk import __version__


setup(name='spdk', version=__version__, packages=find_packages())
