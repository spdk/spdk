
typedef void (*aio_request_fn)(void *arg);

struct aio_request_ctx {
	struct file_disk	*fdisk;
	struct bdev_aio_task	*aio_task;
	struct spdk_thread	*thread;
	aio_request_fn		fn;
	uint64_t		range[2];
	int			status;
	int			errnum;
};

struct aio_request_ctx *create_aio_request_ctx(struct spdk_bdev_io *bdev_io);
void aio_complete(struct spdk_bdev_io *bdev_io, int status);

int aio_remote_request(aio_request_fn fn, void *arg);
void aio_local_request(aio_request_fn fn, void *arg);

void aio_sync_init(void);
void aio_sync_fini(void);
