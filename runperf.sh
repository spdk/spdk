#!/bin/bash
build/examples/perf -q 128 -o 4096 -w randread -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.130.'$1' trsvcid:4420' -t 4
