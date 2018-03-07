# Crypto Virtual Bdev Module {#crypto}

# Introduction {#crypto_intro}

The crypto virtual bdev module can be configured to provide at rest data encryption
for any underlying bdev. The module relies on the DPDK CryptoDev Framework to provide
all cryptographic functionality. The framework provides support for many different software
only cryptographic modules as well hardware assisted support for the Intel QAT board. The
framework also provides support for cipher, hash, authentication and AEAD functions. At this
time the SPDK virtual bdev module supports cipher only as follows:

- AESN-NI Multi Buffer Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC
- Intel(R) QuickAssist (QAT) Crypto Poll Mode Driver: RTE_CRYPTO_CIPHER_AES128_CBC
(Note: QAT is functional however is marked as experimental until the hardware has
been fully integrated with the SPDK CI system.)

Support for other DPDK drivers and capabilities may be added programmatically. Existing
functionality is configured through a .conf file as shown here:

[crypto]
 # CRY <bdev name> <vbdev name> <key> <PMD>
 # key size depends on cipher
 # supported PMD names: crypto_aesni_mb, crypto_qat
 # Note: QAT is experimental while test HW is being setup
 CRY Malloc4 crypto_ram 0123456789123456 crypto_aesni_mb

# Theory of Operation {#crypto_theory}

In order to support using the bdev block offset (LBA) as the initialization vector (IV),
the crypto module break up all I/O into cryto operations of a size equal to the block
size of the underlying bdev.  For example, a 4K I/O to a bdev with a 512B block size,
would result in 8 cryptographic operations.

For reads, the buffer provided to the crypto module will be used as the destination buffer
for unencrypted data.  For writes, however, a temporary scratch buffer is used as the
destination buffer for encryption which is then passed on to the underlying bdev as the
write buffer.  This is done to avoid encrypting the data in the original source buffer which
may cause problems in some use cases.
