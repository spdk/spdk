#!/usr/bin/env python3

import sys
import json
import base64
import struct

buf = sys.stdin.readlines()
json = json.loads(" ".join(buf))
histogram = base64.b64decode(json["histogram"])
bucket_shift = json["bucket_shift"]
tsc_rate = json["tsc_rate"]

print("Latency histogram")
print("==============================================================================")
print("       Range in us     Cumulative    IO count")

so_far = 0
bucket = 0
total = 1

for i in range(0, 64 - bucket_shift):
    for j in range(0, (1 << bucket_shift)):
        index = (((i << bucket_shift) + j) * 8)
        total += int.from_bytes(histogram[index:index + 8], 'little')

for i in range(0, 64 - bucket_shift):
    for j in range(0, (1 << bucket_shift)):
        index = (((i << bucket_shift) + j)*8)
        count = int.from_bytes(histogram[index:index + 8], 'little')
        so_far += count
        last_bucket = bucket

        if i > 0:
            bucket = (1 << (i + bucket_shift - 1))
            bucket += ((j+1) << (i - 1))
        else:
            bucket = j+1

        start = last_bucket * 1000 * 1000 / tsc_rate
        end = bucket * 1000 * 1000 / tsc_rate
        so_far_pct = so_far * 100.0 / total
        if count > 0:
            print("%9.3f - %9.3f: %9.4f%%  (%9u)" % (start, end, so_far_pct, count))
