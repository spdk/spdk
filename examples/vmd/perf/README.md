# Compiling perf on FreeBSD

To use perf test on FreeBSD over NVMe-oF, explicitly link userspace library of HBA. For example, on a setup with Mellanox HBA,

	LIBS += -lmlx5
