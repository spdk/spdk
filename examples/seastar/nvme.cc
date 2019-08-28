#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <iostream>

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"

static struct spdk_nvme_ctrlr *g_ctrlr;
static int g_probe_cnt;

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr_opts *opts)
{
	if (g_probe_cnt++ > 0) {
		return false;
	}

	opts->no_shn_notification = true;
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	std::cout << "Attached to " << trid->traddr << "\n";
	g_ctrlr = ctrlr;
}

struct probe_checker {
	struct spdk_nvme_probe_ctx *probe_ctx;
	bool done;

	probe_checker(struct spdk_nvme_probe_ctx *_probe_ctx) : probe_ctx(_probe_ctx), done(false) {}

	seastar::future<> check() {
		return seastar::do_until([this] { return done; }, [this] {
			if (spdk_nvme_probe_poll_async(probe_ctx) != -EAGAIN) {
				done = true;
			}
			return seastar::make_ready_future<>();
		});
	}
};

struct context {
	struct spdk_nvme_qpair *qpair;
	struct spdk_nvme_ns *ns;
	void *write_buf, *read_buf;
	bool done;
	int io_count;
	const int io_size = 0x1000;

	context() : qpair(NULL), write_buf(NULL), read_buf(NULL), io_count(0) {}

	seastar::future<> start() {
		ns = spdk_nvme_ctrlr_get_ns(g_ctrlr, 1);
		qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
		write_buf = spdk_zmalloc(io_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		read_buf = spdk_zmalloc(io_size, 0, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
		return seastar::make_ready_future<>();
	}

	static void io_done(void *_c, const struct spdk_nvme_cpl *cpl) {
		struct context *c = (struct context *)_c;

		c->done = true;
	}

	seastar::future<> do_io() {
		snprintf((char *)write_buf, io_size, "%s", "Hello world!\n");
		done = false;
		spdk_nvme_ns_cmd_write(ns, qpair, write_buf, 0, io_size / 512, context::io_done, this, 0);
		return seastar::do_until([this] { return done; }, [this] {
			spdk_nvme_qpair_process_completions(qpair, 0);
			return seastar::async([] {});
		}).then([this] {
			done = false;
			spdk_nvme_ns_cmd_read(ns, qpair, read_buf, 0, io_size / 512, context::io_done, this, 0);
			return seastar::do_until([this] { return done; }, [this] {
				spdk_nvme_qpair_process_completions(qpair, 0);
				return seastar::async([] {});
			}).then([this] {
				if (memcmp(read_buf, write_buf, io_size) == 0) {
					printf("%s", (char *)read_buf);
				} else {
					printf("Data miscompare\n");
				}
			});
		});
	}

	seastar::future<> stop() {
		spdk_free(write_buf);
		spdk_free(read_buf);
		spdk_nvme_ctrlr_free_io_qpair(qpair);
		return seastar::make_ready_future<>();
	}
};

seastar::distributed<context> ctx;

seastar::future<> f() {
	struct spdk_nvme_probe_ctx *probe_ctx;
	struct spdk_nvme_transport_id trid;
	struct spdk_env_opts opts;
	int rc;

	spdk_env_opts_init(&opts);
	rc = spdk_env_init(&opts);

	memset(&trid, 0, sizeof(trid));
	trid.trtype = SPDK_NVME_TRANSPORT_PCIE;
	probe_ctx = spdk_nvme_probe_async(&trid, NULL, probe_cb, attach_cb, NULL);

	return seastar::do_with(probe_checker(probe_ctx), [] (auto& checker) {
		return checker.check();
	}).then([] {
		return seastar::async([&] {
			ctx.start().get0();
			ctx.invoke_on_all([] (auto &c) {
				return c.start();
			}).get();
			ctx.invoke_on_all([] (auto &c) {
				return c.do_io();
			}).get();
			ctx.stop().get();
			spdk_nvme_detach(g_ctrlr);
		});
	});
}

int main(int argc, char** argv) {
	seastar::app_template app;
	try {
		app.run(argc, argv, f);
	} catch (...) {
		std::cerr << "Failed to start application: "
			  << std::current_exception() << "\n";
		return 1;
	}
	return 0;
}
