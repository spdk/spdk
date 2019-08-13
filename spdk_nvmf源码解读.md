# 数据结构部分
1. spdk_nvmf_tgt: 包含一个spdk_nvmf_transport的队列用来存储nvmf的所有transport。
2. spdk_nvmf_transport: 
```c++
struct spdk_nvmf_transport {
	// 指向该tansport所属tgt的指针
	struct spdk_nvmf_tgt			*tgt;
	// 该transport的一些操作指针，包含类型，创建销毁等操作
	const struct spdk_nvmf_transport_ops	*ops;
	// 该tranposrt的一些参数
	struct spdk_nvmf_transport_opts		opts;

	/* A mempool for transport related data transfers */
	// 该transport所需的内存从这里申请
	struct spdk_mempool			*data_buf_pool;

	TAILQ_ENTRY(spdk_nvmf_transport)	link;
};
```

# 创建tgt spdk_nvmf_tgt_create
1. 初始化存储该tgt下所有tranposrts的队列。
2. 调用spdk_io_device_register。执行spdk_nvmf_tgt_create_poll_group。io_device为tgt，ctx_buf为spdk_nvmf_poll_group。
   2.1 初试化spdk_nvmf_poll_group的spdk_nvmf_transport_poll_group队列。
   2.2 初始化spdk_nvmf_poll_group的spdk_nvmf_qpair队列。
   2.3 对于该tgt里面的每个transport，将他加入到group中，调用spdk_nvmf_poll_group_add_transport。
        2.3.1: 如果该group中已有的tgroups的transport和他相同则表明这个transport已经在其中了不需要加入。
		2.3.2：<font color="red">调用spdk_nvmf_transport_poll_group_create创建一个该transport的spdk_nvmf_transport_poll_group</font>。
		      2.3.2.1: 调用具体协议层的poll_group_create函数。
			        2.3.2.1.1: 创建一个sock_group
			        2.3.2.1.2: 初始化spdk_nvmf_tcp_poll_group的qpairs队列。
			        2.3.2.1.3: 初始化spdk_nvmf_tcp_poll_group的pending_data_buf_queue队列。
			  2.3.2.2：将该tranposrt赋值给该spdk_nvmf_transport_poll_group的transport属性。
			  2.3.2.3：初始化buf_cache队列。
			  2.3.2.3：获取buf的操作（没有看）。
   2.4 给该spdk_nvmf_poll_group注册poller函数：spdk_nvmf_poll_group_poll
        2.4.1：对于该spdk_nvmf_poll_group中的每一个spdk_nvmf_transport_poll_group执行spdk_nvmf_transport_poll_group_poll，会调用具体协议的pool_group_poll.
		(下次再看吧)

# 创建transport
1. 调用底层的create方法：对于每一个tcp_transport, 初始化一个port的队列。
2. 申请transport的data_buf_pool.
3. spdk_nvmf_tgt_add_transport：将新创建的transport加入到tgt的transport队列中。
    3.1：如果这种类型的transport已经存在，则返回已经存在类型的错误。
	3.2：将tgt赋值给该transport的tgt
	3.3：将该transport插入到tgt的transports队列中。
	3.4：对于该tgt的每个channel调用_spdk_nvmf_tgt_add_transport：将transport加入到spdk_nvmf_poll_group中。
	    3.4.1：通过channel获取spdk_nvmf_poll_group。调用spdk_nvmf_poll_group_add_transport，参见2.3


# accept_poll
1. spdk_nvmf_tgt_accept:对于tgt中的每个transport，调用spdk_nvmf_transport_accept，进而调用具体协议的accept,
   1.1：对于该trannsport的每个port，调用spdk_nvmf_tcp_port_accept
      1.1.1：对该port的listen调用spdk_sock_accept，如果有新的套接字建立，调用_spdk_nvmf_tcp_handle_connect
	     1.1.1.1: 创建一个tqpair对象，然后回调函数。
2. new_qpair:将新建连接添加到nvmf_tgt_poll_group中。
   2.1：获取一个nvmf_tgt_poll_group
   2.2：调用nvmf_tgt_poll_group_add，将qpair添加到nvmf_tgt_poll_group中。
       2.2.1：调用spdk_nvmf_poll_group_add，将qpair添加到该tgt的spdk_nvmf_poll_group中。
	       2.2.1.1：初始化新连接的spdk_nvmf_request队列
		   2.2.1.2：将该spdk_nvmf_poll_group复制给qpair的group
		   2.2.1.3：对于该spdk_nvmf_poll_group中的每个spdk_nvmf_transport_poll_group，如果该poll_group的transport与qpair的transport相同，调用spdk_nvmf_transport_poll_group_add将qpair添加到该spdk_nvmf_transport_poll_group中。
		      2.2.1.3.1:确保该qpair的tansport与该group的transport相同。
		      2.2.1.3.2:调用具体协议的poll_group_add方法。
		      2.2.1.3.2.1: 将qpair的sock添加到该poll_group的sock_group中。
		      2.2.1.3.2.2: spdk_nvmf_tcp_qpair_sock_init：设置sendbuffer,revbufer
		      2.2.1.3.2.3:spdk_nvmf_tcp_qpair_init:初始化发送队列，空闲队列，
		      2.2.1.3.2.4：spdk_nvmf_tcp_qpair_init_mem_resource：初始化内存buf等资源。
		      2.2.1.3.2.5：将tqpair添加到tgroup的qpairs队列中。







