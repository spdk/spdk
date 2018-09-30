
/** \file
 * SPDK BDEV Target
 */

#ifndef SPDK_BDEV_TARGET_H
#define SPDK_BDEV_TARGET_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/nvme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encapsulation and representation of lower-level error conditions
 */
struct spdk_bdev_ret {
	uint64_t status;	///< NVMe command status / completion bits
	uint32_t result;	///< NVMe command error codes
};

int spdk_env_setup(char *config_file);
void spdk_env_unset(void);

struct spdk_bdev_target;
int spdk_bt_open(char *bdev_name, struct spdk_bdev_target **bt);
int spdk_bt_close(struct spdk_bdev_target *bt);

struct spdk_bdev_aio_ctx;
struct spdk_bdev_aio_req;
typedef void (*spdk_bdev_aio_req_complete_cb)(void *cb_arg, int bterrno, struct spdk_bdev_ret *nvm_ret);

typedef void (*spdk_bdev_aio_get_reqs_cb)(void *cb_arg);
typedef void (*spdk_bdev_aio_queue_req_cb)(void *cb_arg);

/* batch io start */
struct spdk_bdev_aio_req {
	union {
		struct {
			void		*pin_buf;
			void		*pin_meta;
			uint64_t	ppa;
			uint32_t	num_lbas;
			uint16_t	io_flags;

			bool		is_read;
		} rw;
		struct {
			struct spdk_nvme_cmd *cmd;
			void		*pin_buf;
			void		*pin_meta;
			uint32_t	data_len;
			uint32_t	md_len;

			bool		is_admin;
		} passthru;
		struct {
			void	*pin_buf;
			void	*pin_meta;
			uint64_t *dst_lba_list;
			uint64_t *src_lba_list;
			uint32_t num_lbas;
			uint32_t io_flags;

			uint8_t io_type;
		}oc_vector;
	} op;

	int req_rc;
	struct spdk_bdev_ret ret;

	spdk_bdev_aio_req_complete_cb user_complete_cb; // func pointer if req has it's own notify routine
	void *complete_cb_arg;

	struct spdk_bdev_aio_ctx *ctx;
	spdk_bdev_aio_queue_req_cb queue_req_fn; // func pointer used to queue req into bdev
	TAILQ_ENTRY(spdk_bdev_aio_req) req_tailq;
};

struct spdk_bdev_aio_get_reqs_ctx{
	bool all;
	int nr_min;
	int nr;
	struct spdk_bdev_aio_req **reqs;

	spdk_bdev_aio_get_reqs_cb get_reqs_cb;
	void *get_reqs_cb_arg;
	int get_reqs_rc;
	struct spdk_bdev_aio_ctx *ctx;
};

struct spdk_bdev_aio_ctx {
	uint32_t bdev_core;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*bdev_spdk_io_channel;
	struct spdk_bdev_target *bt;

	int reqs_submitting;	// number of requests haven't been submitted into bdev
	int reqs_submitted;	// number of requests have been submitted into bdev, but haven't been completed
	int reqs_completed;	// number of requests have been completed, but haven't been realized
	TAILQ_HEAD(req_submitting_list, spdk_bdev_aio_req) submitting_list;
	TAILQ_HEAD(, spdk_bdev_aio_req) completed_list;

	struct spdk_bdev_aio_get_reqs_ctx *get_reqs;
};

int spdk_bdev_aio_ctx_setup(struct spdk_bdev_aio_ctx *ctx, struct spdk_bdev_target *bt);
int spdk_bdev_aio_ctx_get_reqs(struct spdk_bdev_aio_ctx *ctx,
		int nr_min, int nr, struct spdk_bdev_aio_req *reqs[],
		struct timespec *timeout);
int spdk_bdev_aio_ctx_destroy(struct spdk_bdev_aio_ctx *ctx, bool polling_check);
int spdk_bdev_aio_ctx_submit(struct spdk_bdev_aio_ctx *ctx,
		int nr, struct spdk_bdev_aio_req *reqs[]);

void spdk_bdev_aio_req_set_cb(struct spdk_bdev_aio_req *req, spdk_bdev_aio_req_complete_cb cb, void *cb_arg);

void spdk_bdev_aio_req_prep_admin_passthru(struct spdk_bdev_aio_req *req,
		struct spdk_nvme_cmd *cmd, void *pin_buf, size_t data_len);

void spdk_bdev_aio_req_prep_io_passthru(struct spdk_bdev_aio_req *req, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len);




int spdk_bdev_aio_req_admin_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, struct spdk_bdev_ret *ret);
int spdk_bdev_aio_req_io_passthru_sync(struct spdk_bdev_target *bt, struct spdk_nvme_cmd *cmd,
		void *pin_buf, size_t data_len, void *pin_meta, size_t md_len, struct spdk_bdev_ret *ret);

#ifdef __cplusplus
}
#endif

#endif // SPDK_BDEV_TARGET_H
