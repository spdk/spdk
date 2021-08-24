# Userspace DTrace (USDT) {#usdt}

## Package Dependencies

These dependencies are needed for building bpftrace and
the sys/sdt.h header file that SPDK libraries will include
for DTRACE_PROBE macro definitions.

Fedora:
libbpf
gtest-devel
gmock-devel
bcc-devel
systemtap-sdt-devel
llvm-devel
bison
flex

Ubuntu:
systemtap-sdt-dev
libbpfcc-dev
libclang-7-dev
bison
flex

## Building bpftrace

We have found issues with the packaged bpftrace on both Ubuntu 20.04
and Fedora 33.  So bpftrace should be built and installed from source.

```bash
git clone https://github.com/iovisor/bpftrace.git
mkdir bpftrace/build
cd bpftrace/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
sudo make install
```

## bpftrace.sh

bpftrace.sh is a helper script that facilitates running bpftrace scripts
against a running SPDK application.  Here is a typical usage:

```bash
scripts/bpftrace.sh `pidof spdk_tgt` scripts/bpf/nvmf.bt
```

Attaching to USDT probes requires the full path of the binary in the
probe description. SPDK bpftrace scripts can be written with an __EXE__
marker instead of a full path name, and bpftrace.sh will dynamically
replace that string with the full path name using information from procfs.

It is also useful to filter certain kernel events (such as system calls)
based on the PID of the SPDK application.  SPDK bpftrace scripts can be
written with a __PID__ marker, and bpftrace.sh will dynamically replace
that string with the PID provided to the script.

## Configuring SPDK Build

```bash
./configure --with-usdt
```

## Start SPDK application and bpftrace script

From first terminal:

```bash
build/bin/spdk_tgt -m 0xC
```

From second terminal:

```bash
scripts/bpftrace.sh `pidof spdk_tgt` scripts/bpf/nvmf.bt
```

nvmf.bt will print information about nvmf subsystem and poll
group info state transitions.

From third terminal:

```bash
scripts/rpc.py <<EOF
nvmf_create_transport -t tcp
nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 10
nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t tcp -a 127.0.0.1 -s 4420
bdev_null_create null0 1000 512
nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 null0
EOF
```

This creates the nvmf tcp transport, a new nvmf subsystem that listens on a tcp
port, and a null bdev which is added as a namespace to the new nvmf subsystem.

You will see output from the second terminal that looks like this:

```bash
2110.935735: nvmf_tgt reached state NONE
2110.954316: nvmf_tgt reached state CREATE_TARGET
2110.967905: nvmf_tgt reached state CREATE_POLL_GROUPS
2111.235982: nvmf_tgt reached state START_SUBSYSTEMS
2111.253560: nqn.2014-08.org.nvmexpress.discovery change state from INACTIVE to ACTIVE start
2111.260278: nqn.2014-08.org.nvmexpress.discovery on thread 2 state to ACTIVE start
2111.264281: nqn.2014-08.org.nvmexpress.discovery on thread 2 state to ACTIVE done
2111.284083: nqn.2014-08.org.nvmexpress.discovery change state from INACTIVE to ACTIVE done
2111.289197: nvmf_tgt reached state RUNNING
2111.271573: nqn.2014-08.org.nvmexpress.discovery on thread 3 state to ACTIVE start
2111.279787: nqn.2014-08.org.nvmexpress.discovery on thread 3 state to ACTIVE done
2189.921492: nqn.2016-06.io.spdk:cnode1 change state from INACTIVE to ACTIVE start
2189.952508: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE start
2189.959125: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE done
2190.005832: nqn.2016-06.io.spdk:cnode1 change state from INACTIVE to ACTIVE done
2189.969058: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE start
2189.999889: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE done
2197.859104: nqn.2016-06.io.spdk:cnode1 change state from ACTIVE to PAUSED start
2197.879199: nqn.2016-06.io.spdk:cnode1 on thread 2 state to PAUSED start
2197.883416: nqn.2016-06.io.spdk:cnode1 on thread 2 state to PAUSED done
2197.902291: nqn.2016-06.io.spdk:cnode1 change state from ACTIVE to PAUSED done
2197.908939: nqn.2016-06.io.spdk:cnode1 change state from PAUSED to ACTIVE start
2197.912644: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE start
2197.927409: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE done
2197.949742: nqn.2016-06.io.spdk:cnode1 change state from PAUSED to ACTIVE done
2197.890812: nqn.2016-06.io.spdk:cnode1 on thread 3 state to PAUSED start
2197.897233: nqn.2016-06.io.spdk:cnode1 on thread 3 state to PAUSED done
2197.931278: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE start
2197.946124: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE done
2205.859904: nqn.2016-06.io.spdk:cnode1 change state from ACTIVE to PAUSED start
2205.891392: nqn.2016-06.io.spdk:cnode1 on thread 2 state to PAUSED start
2205.896588: nqn.2016-06.io.spdk:cnode1 on thread 2 state to PAUSED done
2205.920133: nqn.2016-06.io.spdk:cnode1 change state from ACTIVE to PAUSED done
2205.905900: nqn.2016-06.io.spdk:cnode1 on thread 3 state to PAUSED start
2205.914856: nqn.2016-06.io.spdk:cnode1 on thread 3 state to PAUSED done
2206.091084: nqn.2016-06.io.spdk:cnode1 change state from PAUSED to ACTIVE start
2206.099222: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE start
2206.105445: nqn.2016-06.io.spdk:cnode1 on thread 2 state to ACTIVE done
2206.119271: nqn.2016-06.io.spdk:cnode1 change state from PAUSED to ACTIVE done
2206.109144: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE start
2206.115636: nqn.2016-06.io.spdk:cnode1 on thread 3 state to ACTIVE done
```

Now stop the bpftrace.sh running in the second terminal, and start
it again with the send_msg.bt script.  This script keeps a count of
functions executed as part of an spdk_for_each_channel or
spdk_thread_send_msg function call.

```bash
scripts/bpftrace.sh `pidof spdk_tgt` scripts/bpf/send_msg.bt
```

From the third terminal, create another null bdev and add it as a
namespace to the cnode1 subsystem.

```bash
scripts/rpc.py <<EOF
bdev_null_create null1 1000 512
nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 null1
EOF
```

Now Ctrl-C the bpftrace.sh in the second terminal, and it will
print the final results of the maps.

```bash
@for_each_channel[subsystem_state_change_on_pg]: 2

@send_msg[_finish_unregister]: 1
@send_msg[_call_completion]: 2
@send_msg[put_io_channel]: 4
@send_msg[_call_channel]: 4
```

## TODOs and known limitations

- add definitions for common nvmf data structures - then we can pass the subsystem pointer
  itself as a probe parameter and let the script decide which fields it wants to access
  (Note: these would need to be kept up-to-date with the C definitions of the struct - it is
  not possible to include the header files in a bpftrace script)
- investigate using pahole to generate data structure definitions that can be included in
  bpftrace scripts; this would allow us to pass the subsystem pointer itself as a probe
  argument, and let the script decide which fields it wants to access; for example,
  `pahole -E -C spdk_nvmf_subsystem build/bin/spdk_tgt` gets us close to what we need,
  but there are some limiters:
  - our structures have char arrays (not char pointers) for things like subnqn; large
    arrays like these cannot currently be passed to bpftrace printf without generating
    a stack space error (probe points are limited to 512 bytes of stack); we could
    modify SPDK to have the char array for storage, and a char pointer that points to
    that storage, the latter could easily then be used in bpftrace scripts
  - our structures include fields with their enum types instead of int; bpftrace will
    complain it does not know about the enum (pahole doesn't print out enum
    descriptions); information on enums can be found in the applications .debug_info
    section, but we would need something that can convert that into a file we can
    include in a bpftrace script
- Note that bpftrace prints are not always printed in exact chronological order; this can
  be seen especially with spdk_for_each_channel iterations, where we execute trace points
  on multiple threads in a very short period of time, and those may not get printed to the
  console in exact order; this is why the nvmf.bt script prints out a msec.nsec timestamp
  so that the user can understand the ordering and even pipe through sort if desired
- flesh out more DTrace probes in the nvmf code
