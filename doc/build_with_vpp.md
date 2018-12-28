# Vector Packet Processing {#vpp}

VPP (part of [Fast Data - Input/Output](https://fd.io/) project) is an extensible
userspace framework providing networking functionality. It is build on idea of
packet processing graph (see [What is VPP?](https://wiki.fd.io/view/VPP/What_is_VPP?)).

A detailed instructions for **simplified steps 1-3** below, can be found on
VPP [Quick Start Guide](https://wiki.fd.io/view/VPP).

*SPDK supports VPP version 18.01.1.*

##  1. Building VPP (optional) {#vpp_build}

*Please skip this step if using already built packages.*

Clone and checkout VPP
~~~
git clone https://gerrit.fd.io/r/vpp && cd vpp
git checkout v18.01.1
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
make pkg-deb
~~~

Packages can be found in `vpp/build-root/` directory.

For more in depth instructions please see Building section in
[VPP documentation](https://wiki.fd.io/view/VPP/Pulling,_Building,_Running,_Hacking_and_Pushing_VPP_Code#Building)

*Please note: VPP 18.01.1 does not support OpenSSL 1.1. It is suggested to install a compatibility package
for compilation time.*
~~~
sudo dnf install -y --allowerasing compat-openssl10-devel
~~~
*Then reinstall latest OpenSSL devel package:*
~~~
sudo dnf install -y --allowerasing openssl-devel
~~~

## 2. Installing VPP {#vpp_install}

Packages can be installed from distribution repository or built in previous step.
Minimal set of packages consists of `vpp`, `vpp-lib` and `vpp-devel`.

*Note: Please remove or modify /etc/sysctl.d/80-vpp.conf file with appropriate values
dependent on number of hugepages that will be used on system.*

## 3. Running VPP {#vpp_run}

VPP takes over any network interfaces that were bound to userspace driver,
for details please see DPDK guide on
[Binding and Unbinding Network Ports to/from the Kernel Modules](http://dpdk.org/doc/guides/linux_gsg/linux_drivers.html#binding-and-unbinding-network-ports-to-from-the-kernel-modules).

VPP is installed as service and disabled by default. To start VPP with default config:
~~~
sudo systemctl start vpp
~~~

Alternatively, use `vpp` binary directly
~~~
sudo vpp unix {cli-listen /run/vpp/cli.sock}
~~~

A usefull tool is `vppctl`, that allows to control running VPP instance.
Either by entering VPP configuration prompt
~~~
sudo vppctl
~~~

Or, by sending single command directly. For example to display interfaces within VPP:
~~~
sudo vppctl show interface
~~~

### Example: Tap interfaces on single host

For functional test purpose a virtual tap interface can be created,
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

## 4. Building SPDK with VPP {#vpp_built_into_spdk}

Support for VPP can be built into SPDK by using configuration option.
~~~
configure --with-vpp
~~~

Alternatively, directory with built libraries can be pointed at
and will be used for compilation instead of installed packages.
~~~
configure --with-vpp=/path/to/vpp/repo/build-root/vpp
~~~

## 5. Running SPDK with VPP {#vpp_running_with_spdk}

VPP application has to be started before SPDK iSCSI target,
in order to enable usage of network interfaces.
After SPDK iSCSI target initialization finishes,
interfaces configured within VPP will be available to be configured as portal addresses.
Please refer to @ref iscsi_rpc.
