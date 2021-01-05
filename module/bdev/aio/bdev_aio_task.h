
struct bdev_aio_task {
	struct iocb			iocb;
	uint64_t			len;
	struct bdev_aio_io_channel	*ch;
	TAILQ_ENTRY(bdev_aio_task)	link;
};
