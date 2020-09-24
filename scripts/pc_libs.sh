#!/usr/bin/env bash
cat << EOF
Description: $3 libraries used by SPDK
Name: $4
Version: 1.0
Libs: $1
Libs.private: $2
EOF
