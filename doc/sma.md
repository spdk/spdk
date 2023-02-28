# Storage Management Agent {#sma}

Storage Management Agent (SMA) is a service providing a gRPC interface for
orchestrating SPDK applications.  It's a standalone application that allows
users to create and manage various types of devices (e.g. NVMe, virtio-blk,
etc.).  The major difference between SMA's API and the existing SPDK-RPC
interface is that it's designed to abstract the low level details exposed by
SPDK-RPCs, which enables it to be more easily consumed by orchestration
frameworks, such as k8s or OpenStack.  This is especially important for
deployments on IPUs (Infrastructure Processing Unit), which usually require a
lot of hardware-specific options.

## Interface

The interface is defined in a protobuf files located in `proto`
directory.  The generic interface common to all types of devices is defined in
`sma.proto` file, while device-specific options are defined in their separate
files (e.g. `nvme.proto` for NVMe).

Currently, the interface consists of four methods.  Additionally, it defines two
main types of objects: volumes and devices.  A volume is a representation of
some storage media.  It is equivalent to a SPDK bdev and/or an NVMe namespace
and can exist even if it's not presented to the host system.  A device is
usually a virtual/physical PCIe function that is exposed to a host.  It is
capable of presenting one or more volumes (depending on the type of the device)
to a host.

The following sections provide a high-level description of each method.  For
more details, consult the protobuf definitions.

### CreateDevice

This method creates a device.  If a device with given parameters already exists,
it becomes a no-op and returns a handle to that device.

Input:

- `volume`: Volume parameters describing a volume to immediately attach to the
  created device.  This field may be optional for some device types (e.g. NVMe),
  while it may be required for others (e.g. virtio-blk).
- `params`: Device-specific parameters.  The type of this structure determines
  the type of device to create.

Output:

- `handle`: Opaque handle identifying the device.

### DeleteDevice

This method deletes a device.  Volumes that are still attached to a device being
deleted will be automatically detached.

Input:

- `handle`: Device handle obtained from `CreateDevice`.

### AttachVolume

This method creates a volume and attaches it to a device exposing it to the
host.  It might lead to establishing a connection to remote storage target.
However, this is not always the case, even if the volume is remote.  For
instance, if a volume describes an NVMe namespace, it might already be connected
if another volume on the same subsystem was created previously.  It may be
unsupported by some types of devices (e.g. virtio-blk).

Input:

- `volume`: Parameters describing the volume to attach.  The type of this
  structure determines the method to create it (e.g. direct NVMe-oF connection,
  NVMe-oF through discovery service, iSCSI, etc.).
- `device_handle`: Device handle obtained from `CreateDevice`.

### DetachVolume

This method detaches a volume from a device making it unavailable to the host.
It may be unsupported by some types of devices (e.g. virtio-blk).

Input:

- `volume_id`: Volume UUID/GUID.
- `device_handle`: Device handle obtained from `CreateDevice`.

## Running and Configuration

In order to run SMA, SPDK needs to be configured with the `--with-sma` flag.
Then, SMA can be started using a script located in `scripts/sma.py`.  It
requires a YAML configuration file that specifies which types of devices to
service, as well as several other options (e.g. listen address, SPDK-RPC socket,
etc.).  Device types not listed in the configuration will be disabled and it
won't be possible to manage them.  Below is an example configuration enabling
two device types (NVMe/vfiouser and vhost-blk):

```yaml
address: 'localhost'
socket: '/var/tmp/spdk.sock'
port: 8080
devices:
  - name: 'vfiouser'
    params:
      root_path: '/var/tmp/vfiouser'
      bus: 'bus0'
      address: '127.0.0.1'
      port: 4444
  - name: 'vhost-blk'
```

## Plugins

SMA provides a way to load external plugins implementing support for specific
device types.  A plugin will be loaded if it's specified in the `SMA_PLUGINS`
environment variable (multiple plugins are separated with a colon) or if it's
specified in the `plugins` section of the config file.  For example, the
following two methods are equivalent:

```sh
$ SMA_PLUGINS=plugin1:plugin2 scripts/sma.py

$ cat sma.yaml
address: 'localhost'
port: 8080
plugins:
  - 'plugin1'
  - 'plugin2'
devices:
  - name: 'device-from-plugin1'
  - name: 'device-from-plugin2'
$ scripts/sma.py -c sma.yaml
```

Each plugin needs to be in the python search path (either in one of the default
directories or added to `PYTHONPATH`).

A plugin is required to define a global variable called `devices` storing a list
of classes deriving from `spdk.sma.DeviceManager`.  This base class define the
interface each device needs to implement.  Additionally, each DeviceManager
needs to define a unique name that will be used to identify it in config file as
well as the name of the protocol it supports.  There can be many DeviceManagers
supporting the same protocol, but only one can be active at a time.  The name of
the protocol shall match the type specified in `CreateDeviceRequest.params`
(e.g. "nvme", "virtio_blk", etc.), as it'll be used to select the DeviceManager
to handle a gRPC request.  Finally, a DeviceManager needs to implement the
`own_device()` method returning a boolean value indicating whether a given
device handle is owned by that DeviceManager.
