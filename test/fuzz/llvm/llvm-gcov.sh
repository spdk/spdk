#!/bin/bash
#  SPDX-License-Identifier: BSD-3-Clause
#  All rights reserved.

# Wrapper for llvm gcov, --gcov-tool
exec llvm-cov gcov "$@"
