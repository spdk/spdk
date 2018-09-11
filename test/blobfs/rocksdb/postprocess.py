#!/usr/bin/env python3
from collections import namedtuple
from itertools import islice
import operator
import sys

total_samples = 0
thread_module_samples = {}
function_module_samples = {}
module_samples = {}
threads = set()

ThreadModule = namedtuple('ThreadModule', ['thread', 'module'])
FunctionModule = namedtuple('FunctionModule', ['function', 'module'])

with open(sys.argv[1] + "/" + sys.argv[2] + ".perf.txt") as f:
    for line in f:
        fields = line.split()
        total_samples += int(fields[1])
        key = ThreadModule(fields[2], fields[3])
        thread_module_samples.setdefault(key, 0)
        thread_module_samples[key] += int(fields[1])
        key = FunctionModule(fields[5], fields[3])
        function_module_samples.setdefault(key, 0)
        function_module_samples[key] += int(fields[1])
        threads.add(fields[2])

        key = fields[3]
        module_samples.setdefault(key, 0)
        module_samples[key] += int(fields[1])

for thread in sorted(threads):
    thread_pct = 0
    print("")
    print("Thread: {:s}".format(thread))
    print(" Percent      Module")
    print("============================")
    for key, value in sorted(list(thread_module_samples.items()), key=operator.itemgetter(1), reverse=True):
        if key.thread == thread:
            print("{:8.4f}      {:20s}".format(float(value) * 100 / total_samples, key.module))
            thread_pct += float(value) * 100 / total_samples
    print("============================")
    print("{:8.4f}       Total".format(thread_pct))

print("")
print(" Percent      Module               Function")
print("=================================================================")
for key, value in islice(sorted(list(function_module_samples.items()), key=operator.itemgetter(1), reverse=True), 100):
    print(("{:8.4f}      {:20s} {:s}".format(float(value) * 100 / total_samples, key.module, key.function)))

print("")
print("")
print(" Percent      Module")
print("=================================")
for key, value in sorted(list(module_samples.items()), key=operator.itemgetter(1), reverse=True):
    print("{:8.4f}      {:s}".format(float(value) * 100 / total_samples, key))

print("")
with open(sys.argv[1] + "/" + sys.argv[2] + "_db_bench.txt") as f:
    for line in f:
        if "maxresident" in line:
            fields = line.split()
            print("Wall time elapsed: {:s}".format(fields[2].split("e")[0]))
            print("CPU utilization: {:s}".format(fields[3].split('C')[0]))
            user = float(fields[0].split('u')[0])
            system = float(fields[1].split('s')[0])
            print("User:   {:8.2f} ({:5.2f}%)".format(user, user * 100 / (user + system)))
            print("System: {:8.2f} ({:5.2f}%)".format(system, system * 100 / (user + system)))

print("")
