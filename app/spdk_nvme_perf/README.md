# Compiling perf on FreeBSD

To use spdk_nvme_perf on FreeBSD over NVMe-oF, explicitly link userspace library of HBA. For example, on a setup with Mellanox HBA,

```make
	LIBS += -lmlx5
```
