Start `nvmf_tgt` with 1 name space:

	LD_LIBRARY_PATH=../muser/build/dbg/lib/ app/nvmf_tgt/nvmf_tgt -L nvme -L nvmf -L nvmf_muser &
	scripts/rpc.py nvmf_create_transport -t MUSER
	scripts/rpc.py nvmf_subsystem_create -a nqn.2019-07.io.spdk.muser:00000000-0000-0000-0000-000000000000
	scripts/rpc.py construct_malloc_bdev -b thanos $((1024)) 512
	scripts/rpc.py nvmf_subsystem_add_ns -n 1 nqn.2019-07.io.spdk.muser:00000000-0000-0000-0000-000000000000 thanos
	scripts/rpc.py nvmf_subsystem_add_listener -t MUSER -a "00000000-0000-0000-0000-000000000000" -s 0 nqn.2019-07.io.spdk.muser:00000000-0000-0000-0000-000000000000
