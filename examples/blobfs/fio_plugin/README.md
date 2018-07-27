# Compile

To compile the BlobFS fio plugin with fio, as same as the bdev fio plugin.
1. Download the fio source.
2. Compiling SPDK with fio path.

See the README of bdev fio plugin for details.

# Usage
1. Generate configuration file of device by scripts/gen_nvme.sh.
2. Make BlobFS by mkfs provided in the repo test/blobfs/mkfs.
3. Run fio command with the fio plugin by specify the plugin binary using LD_PRELOAD and set ioengine=spdk_blobfs in the fio configuration file (see exampleconfig.fio in the same directory as this README).

	<path_to>/gen_nvme.sh >> <spdk_path>/examples/blobfs/fio_plugin/bdev.conf
	<path_to>/mkfs <spdk_path>/examples/blobfs/fio_plugin/bdev.conf Nvme0n1
	LD_PRELOAD=<spdk_path>/examples/blobfs/fio_plugin/fio_plugin fio example_config.fio -filename=Mvme0n1
	
** Note: only numjobs=1 supported now **
