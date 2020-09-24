#!/usr/bin/env bash
cat << EOF
Description: SPDK $1 modules
Name: spdk_$1_modules
Version: 1.0
Requires: $2
EOF
