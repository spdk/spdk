#!/usr/bin/env bash

function configure_performance() {
	echo -n "Placing all CPUs in performance mode..."
	for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		echo -n performance > $governor
	done
	echo "Done"

	if [ -f "/sys/devices/system/cpu/intel_pstate/no_turbo" ]; then
		echo -n "Disabling Turbo Boost..."
		echo -n 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
		echo "Done"
	fi

	echo -n "Disabling irqbalance service..."
	service irqbalance stop 2> /dev/null
	echo "Done"

	echo -n "Moving all interrupts off of core 0..."
	count=$(($(nproc) / 4))
	cpumask="e"
	for ((i = 1; i < count; i++)); do
		if [ $((i % 8)) -eq 0 ]; then
			cpumask=",$cpumask"
		fi
		cpumask="f$cpumask"
	done
	for file in /proc/irq/*/smp_affinity; do
		echo "$cpumask" > $file 2> /dev/null
	done
	echo "Done"

	echo -n "Configuring kernel blk-mq for NVMe SSDs..."
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
}

function reset_performance() {
	echo -n "Placing all CPUs in powersave mode..."
	for governor in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
		echo -n powersave > $governor
	done
	echo "Done"

	if [ -f "/sys/devices/system/cpu/intel_pstate/no_turbo" ]; then
		echo -n "Enabling Turbo Boost..."
		echo -n 0 > /sys/devices/system/cpu/intel_pstate/no_turbo
		echo "Done"
	fi

	echo -n "Enabling irqbalance service..."
	service irqbalance start 2> /dev/null
	echo "Done"
}

if [ "$1" = "reset" ]; then
	reset_performance
else
	configure_performance
fi
