# FIPS Compliance {#fips}

SPDK does not implement any cryptography itself, and for use cases requiring cryptographic
functions it relies on external components. Therefore, SPDK alone has not acquired any FIPS
(Federal Information Processing Standards) certification, but a reasonable effort has been made to
check, if SPDK can be a part of a FIPS certified system as one of the components. Any FIPS
certification of such a system, however, is a responsibility of the system integrator or builder.
The dependencies are:

* [Intel(R) Intelligent Storage Acceleration Library Crypto Version (isa-l-crypto)](https://github.com/intel/isa-l_crypto),
* [Intel(R) Multi-Buffer Crypto for IPSec (intel-ipsec-mb)](https://github.com/intel/intel-ipsec-mb),
* [Intel QuickAssist Technology (QAT) Crypto Driver](https://doc.dpdk.org/guides/cryptodevs/qat.html),
* [NVIDIA MLX5 Crypto Driver](https://doc.dpdk.org/guides/cryptodevs/mlx5.html),
* [OpenSSL](https://www.openssl.org/).

Intel QAT driver, NVIDIA MLX5 driver and intel-ipsec-mb are delivered by DPDK as
[Crypto Device Drivers](https://doc.dpdk.org/guides/cryptodevs/).
MLX5 usage is supported for both DPDK and SPDK.

SPDK can be compatible with FIPS, if all enabled dependencies are operating in a FIPS approved
state.

## DPDK

To ensure the system using SPDK can apply for FIPS certification, please use FIPS certified
versions of DPDK and intel-ipsec-mb - you may include a custom DPDK version via `--with-dpdk`
configuration flag. Please also make sure, that FIPS-certified hardware and firmware is used
(e.g. Intel QAT).

## isa-l-crypto

The isa-l-crypto library has not yet acquired the FIPS certification. In order for SPDK to be
included in a FIPS certified system, please do not use the software Acceleration Framework module
for encryption/decryption.

## OpenSSL

SPDK uses various functions from OpenSSL library to perform tasks like key derivation in
[NVMe TCP](https://github.com/spdk/spdk/blob/master/include/spdk_internal/nvme_tcp.h) and TLS
handshake in [socket module](https://github.com/spdk/spdk/blob/master/module/sock/posix/posix.c).
OpenSSL delivers code implemenations via
[providers](https://www.openssl.org/docs/man3.0/man7/provider.html).

One of such providers delivers Federal Information Processing Standards (FIPS) compliant functions,
called [FIPS provider](https://www.openssl.org/docs/man3.0/man7/OSSL_PROVIDER-FIPS.html), and if
[set up correctly](https://wiki.openssl.org/index.php/OpenSSL_3.0#Using_the_FIPS_module_in_SSL.2FTLS),
will include only function implementations that fall under FIPS 140-2.

SPDK provides a test in `test/nvmf/fips/fips.sh` that includes OpenSSL configuration via
[OPENSSL_CONF](https://www.openssl.org/docs/man1.1.1/man5/config.html). As seen in that test, the
configuration will differ between some operating systems (see [Fedora38](https://www.redhat.com/en/blog/openssl-fips-140-2-upstream-140-3-downstream)
for example). Correctly configuring OpenSSL FIPS is user responsibility (see [this link](https://github.com/openssl/openssl/blob/master/README-FIPS.md)
for reference on how to enable FIPS provider). OpenSSL documentation states that "It is undefined
which implementation of an algorithm will be used if multiple implementations are available", so we
strongly recommend to use FIPS + base provider combination exclusively to ensure FIPS compliance
([OpenSSL doc](https://www.openssl.org/docs/man3.0/man7/fips_module.html#Programmatically-loading-the-FIPS-module-default-library-context)).

To ensure the system using SPDK can apply for FIPS certification, please use OpenSSL versions
3.0.0+ only. In terms of generation and handling of the pre-shared keys for TLS, it is recommended
to ensure compliance with FIPS standards – in particular: NIST Special Publication 800-133,
Revision 2.

## Example crypto configuration

Below are RPCs that can be sent to SPDK application to configure FIPS compliant cryptography.
See `test/bdev/blockdev.sh` for example usage and @ref jsonrpc for explanations of RPC commands.

Configuration of dpdk_cryptodev in acceleration framework should first initialize the module, then
select cryptodev device (crypto_aesni_mb, crypto_qat or mlx5_pci) and finally assign the
dpdk_cryptodev driver to specific operations:

```bash
./scripts/rpc.py dpdk_cryptodev_scan_accel_module
./scripts/rpc.py dpdk_cryptodev_set_driver -d crypto_aesni_mb
./scripts/rpc.py accel_assign_opc -o encrypt -m dpdk_cryptodev
./scripts/rpc.py accel_assign_opc -o decrypt -m dpdk_cryptodev
```

MLX5 configuration, which depends on 3rd party FIPS-certified hardware, should contain only module
initialization ad operation assignment:

```bash
./scripts/rpc.py mlx5_scan_accel_module
./scripts/rpc.py accel_assign_opc -o encrypt -m mlx5
./scripts/rpc.py accel_assign_opc -o decrypt -m mlx5
```

## Symmetric encryption keys

It is recommended to use the 256 bit keys with symmetric encryption algorithms. For AES-XTS
specifically, the supplied Key1 must be different from Key2.
