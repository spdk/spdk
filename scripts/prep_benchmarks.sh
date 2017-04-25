#!/usr/bin/env bash

echo -n "Placing all CPUs in performance mode..."
for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
	echo -n performance > $governor
done
echo "Done"

echo -n "Disabling irqbalance service..."
service irqbalance stop 2> /dev/null
echo "Done"

echo -n "Moving all interrupts off of core 0..."
count=`expr $(nproc) / 4`
cpumask="e"
for ((i=1; i<$count; i++)); do
	if [ `expr $i % 8` -eq 0 ]; then
		cpumask=",$cpumask"
	fi
	cpumask="f$cpumask"
done
for file in /proc/irq/*/smp_affinity; do
	echo "$cpumask" > $file 2> /dev/null
done
echo "Done"

echo -n "Configuring kernel blk-mq for for NVMe SSDs..."
for queue in /sys/block/nvme*n*/queue; do
	if [ -f "$queue/nomerges" ]; then
		echo "1" > $queue/nomerges
	fi

	if [ -f "$queue/io_poll" ]; then
		echo "1" > $queue/io_poll
	fi

	if [ -f "$queue/io_poll_delay" ]; then
		echo "-1" > $queue/io_poll_delay
	fi
done
echo "Done"
