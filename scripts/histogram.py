#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2019 Intel Corporation
#  All rights reserved.
#

import sys
import json
import base64
import struct

buf = sys.stdin.readlines()
json = json.loads(" ".join(buf))
histogram = base64.b64decode(json["histogram"])
granularity = json["granularity"]
min_range = json["min_range"]
max_range = json["max_range"]
tsc_rate = json["tsc_rate"]

print("Latency histogram")
print("==============================================================================")
print("       Range in us     Cumulative    IO count")

so_far = 0
bucket = 0
total = 1

for i in range(0, max_range - min_range):
    for j in range(0, (1 << granularity)):
        index = (((i << granularity) + j) * 8)
        total += int.from_bytes(histogram[index:index + 8], 'little')

for i in range(0, max_range - min_range):
    for j in range(0, (1 << granularity)):
        index = (((i << granularity) + j)*8)
        count = int.from_bytes(histogram[index:index + 8], 'little')
        so_far += count
        last_bucket = bucket

        if i > 0:
            bucket = (1 << (i + granularity + min_range - 1))
            bucket += ((j+1) << (i + min_range - 1))
        else:
            bucket = (j+1) << min_range

        start = last_bucket * 1000 * 1000 / tsc_rate
        end = bucket * 1000 * 1000 / tsc_rate
        so_far_pct = so_far * 100.0 / total
        if count > 0:
            print("%9.3f - %9.3f: %9.4f%%  (%9u)" % (start, end, so_far_pct, count))
