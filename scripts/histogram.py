#!/usr/bin/env python3

import sys
import json
import base64

endianness = 'little'
bytes_per_value = 8

if __name__ == "__main__":
    buf = sys.stdin.readlines()

    try:
        json = json.loads(" ".join(buf))

        histogram = base64.b64decode(json["histogram"])
        bucket_shift = json["bucket_shift"]
        tsc_rate = json["tsc_rate"]

        so_far = 0
        bucket = 0
        total = 0

        for i in range(0, 64 - bucket_shift):
            for j in range(0, (1 << bucket_shift)):
                index = ((i << bucket_shift) + j) * bytes_per_value
                total += int.from_bytes(histogram[index:index + bytes_per_value], endianness)

        if total == 0:
            print("No data in histogram")
            exit(0)

        print("Latency histogram")
        print("==============================================================================")
        print("       Range in us     Cumulative    IO count")
        for i in range(0, 64 - bucket_shift):
            for j in range(0, (1 << bucket_shift)):
                index = ((i << bucket_shift) + j) * bytes_per_value
                count = int.from_bytes(histogram[index:index + bytes_per_value], endianness)
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

    except ValueError:
        print("Cannot display histogram: Incorrect JSON input")
        exit(1)
    except KeyError as error:
        print("Cannot display histogram: %s is not defined" % error)
        exit(1)
