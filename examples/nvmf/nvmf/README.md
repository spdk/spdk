This example is used to show how to use the nvmf lib. So this example's usage is very similar with nvmf_tgt.
1, we need a configuration file.
   we can find the example of configuration file from etc/spdk/nvmf.conf.in

usage:
	1, rum the example
	./nvmf -c nvmf_conf.io -m 0xf
	2, run an initiator to connect the nvmf example and test the IOs.
