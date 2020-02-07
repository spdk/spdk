# Vector Packet Processing {#vpp_integration}

VPP (part of [Fast Data - Input/Output](https://fd.io/) project) is an extensible
userspace framework providing networking functionality. It is built around the concept of
packet processing graph (see [What is VPP?](https://wiki.fd.io/view/VPP/What_is_VPP?)).

Detailed instructions for **simplified steps 1-3** below, can be found on
VPP [Quick Start Guide](https://wiki.fd.io/view/VPP).

*SPDK supports VPP version 19.04.2.*

# 1. Building VPP (optional) {#vpp_build}

*Please skip this step if using already built packages.*

Clone and checkout VPP
~~~
git clone https://gerrit.fd.io/r/vpp && cd vpp
git checkout v19.04.2
~~~

Install VPP build dependencies
~~~
make install-dep
~~~

Build and create .rpm packages
~~~
make pkg-rpm
~~~

Alternatively, build and create .deb packages
~~~
make bootstrap && make pkg-deb
~~~

Packages can be found in `vpp/build-root/` directory.

For more in depth instructions please see Building section in
[VPP documentation](https://wiki.fd.io/view/VPP/Pulling,_Building,_Running,_Hacking_and_Pushing_VPP_Code#Building)

# 2. Installing VPP {#vpp_install}

Packages can be installed from a distribution repository or built in previous step.
Minimal set of packages consists of `vpp`, `vpp-lib` and `vpp-devel`.

*Note: Please remove or modify /etc/sysctl.d/80-vpp.conf file with appropriate values
dependent on number of hugepages that will be used on system.*

# 3. Running VPP {#vpp_run}

VPP takes over any network interfaces that were bound to userspace driver,
for details please see DPDK guide on
[Binding and Unbinding Network Ports to/from the Kernel Modules](http://dpdk.org/doc/guides/linux_gsg/linux_drivers.html#binding-and-unbinding-network-ports-to-from-the-kernel-modules).

VPP is installed as service and disabled by default. To start VPP with default config:
~~~
sudo systemctl start vpp
~~~

Alternatively, use `vpp` binary directly
~~~
sudo vpp unix {cli-listen /run/vpp/cli.sock} session { evt_qs_memfd_seg } socksvr { socket-name /run/vpp-api.sock }
~~~

# 4. Configure VPP {#vpp_config}

VPP can be configured using a VPP startup file and the `vppctl` command; By default, the VPP startup file is `/etc/vpp/startup.conf`, however, you can pass any file with the `-c` vpp command argument.

## Startup configuration

Some key values from iSCSI point of view includes:

CPU section (`cpu`):

- `main-core <lcore>` -- logical CPU core used for main thread.
- `corelist-workers <lcore list>` -- logical CPU cores where worker threads are running.

DPDK section (`dpdk`):

- `num-rx-queues <num>` -- number of receive queues.
- `num-tx-queues <num>` -- number of transmit queues.
- `dev <PCI address>` -- whitelisted device.

Session section (`session`):

- `evt_qs_memfd_seg` -- uses a memfd segment for event queues. This is required for SPDK.

Socket server session (`socksvr`):

- `socket-name <path>` -- configure API socket filename (curently SPDK uses default path `/run/vpp-api.sock`).

Plugins section (`plugins`):

- `plugin <plugin name> { [enable|disable] }` -- enable or disable VPP plugin.

### Example

~~~
unix {
	nodaemon
	cli-listen /run/vpp/cli.sock
}
cpu {
	main-core 1
}
session {
	evt_qs_memfd_seg
}
socksvr {
	socket-name /run/vpp-api.sock
}
plugins {
	plugin default { disable }
	plugin dpdk_plugin.so { enable }
}
~~~

## vppctl command tool

The `vppctl` command tool allows users to control VPP at runtime via a command prompt
~~~
sudo vppctl
~~~

Or, by sending single command directly. For example to display interfaces within VPP:
~~~
sudo vppctl show interface
~~~

Useful commands:

- `show interface` -- show interfaces settings, state and some basic statistics.
- `show interface address` -- show interfaces state and assigned addresses.

- `set interface ip address <VPP interface> <Address>` -- set interfaces IP address.
- `set interface state <VPP interface> [up|down]` -- bring interface up or down.

- `show errors` -- show error counts.

## Example: Configure two interfaces to be available via VPP

We want to configure two DPDK ports with PCI addresses 0000:09:00.1 and 0000:0b:00.1
to be used as portals 10.0.0.1/24 and 10.10.0.1/24.

In the VPP startup file (e.g. `/etc/vpp/startup.conf`), whitelist the interfaces
by specifying PCI addresses in section dpdk:
~~~
	dev 0000:09:00.1
	dev 0000:0b:00.1
~~~

Bind PCI NICs to UIO driver (`igb_uio` or `uio_pci_generic`).

Restart vpp and use vppctl tool to verify interfaces.
~~~
$ vppctl show interface
              Name               Idx    State  MTU (L3/IP4/IP6/MPLS)     Counter          Count

FortyGigabitEthernet9/0/1         1     down         9000/0/0/0
FortyGigabitEthernetb/0/1         2     down         9000/0/0/0
~~~

Set appropriate addresses and bring interfaces up:
~~~
$ vppctl set interface ip address FortyGigabitEthernet9/0/1 10.0.0.1/24
$ vppctl set interface state FortyGigabitEthernet9/0/1 up
$ vppctl set interface ip address FortyGigabitEthernetb/0/1 10.10.0.1/24
$ vppctl set interface state FortyGigabitEthernetb/0/1 up
~~~

Verify configuration:
~~~
$ vppctl show interface address
FortyGigabitEthernet9/0/1 (up):
  L3 10.0.0.1/24
FortyGigabitEthernetb/0/1 (up):
  L3 10.10.0.1/24
~~~

Now, both interfaces are ready to use. To verify conectivity you can ping
10.0.0.1 and 10.10.0.1 addresses from another machine.

## Example: Tap interfaces on single host

For functional test purposes a virtual tap interface can be created,
so no additional network hardware is required.
This will allow network communication between SPDK iSCSI target using VPP end of tap
and kernel iSCSI initiator using the kernel part of tap. A single host is used in this scenario.

Create tap interface via VPP
~~~
    vppctl tap connect tap0
    vppctl set interface state tapcli-0 up
    vppctl set interface ip address tapcli-0 10.0.0.1/24
    vppctl show int addr
~~~

Assign address on kernel interface
~~~
    sudo ip addr add 10.0.0.2/24 dev tap0
    sudo ip link set tap0 up
~~~

To verify connectivity
~~~
    ping 10.0.0.1
~~~

# 5. Building SPDK with VPP {#vpp_built_into_spdk}

Support for VPP can be built into SPDK by using configuration option.
~~~
configure --with-vpp
~~~

Alternatively, directory with built libraries can be pointed at
and will be used for compilation instead of installed packages.
~~~
configure --with-vpp=/path/to/vpp/repo/build-root/install-vpp-native/vpp
~~~

# 6. Running SPDK with VPP {#vpp_running_with_spdk}

VPP application has to be started before SPDK application, in order to enable
usage of network interfaces. For example, if you use SPDK iSCSI target or
NVMe-oF target, after the initialization finishes, interfaces configured within
VPP will be available to be configured as portal addresses.

Moreover, you do not need to specifiy which TCP sock implementation (e.g., posix,
VPP) to be used through configuration file or RPC call. Since SPDK program
automatically determines the protocol according to the configured portal addresses
info. For example, you can specify a Listen address in NVMe-oF subsystem
configuration such as "Listen TCP 10.0.0.1:4420". SPDK programs automatically
uses different implemenation to listen this provided portal info via posix or
vpp implemenation(if compiled in SPDK program), and only one implementation can
successfully listen on the provided portal.
