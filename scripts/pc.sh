#!/usr/bin/env bash
cat << EOF
Description: SPDK $2 library
Name: spdk_$2
Version: $3
Libs: -L$1/lib  -lspdk_$2
Requires: $4
Libs.private: $5
Cflags: -I$1/include
EOF
