


#include "spdk/stdinc.h"

#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk/bdev_target.h"

#include <getopt.h>

#include "ocssd_dev.h"

static int ocssd_dev_init(struct spdk_bdev_target *bt, struct ocssd_dev **_dev);
static int ocssd_sblk_construct(struct ocssd_dev *dev, struct ocssd_sblk **_sblk,
		int lun_start, int lun_end, int chunk_idx);
static int ocssd_sblk_align(struct ocssd_sblk *sblk);
static int ocssd_sblk_check(struct ocssd_sblk *sblk);
static int ocssd_sblk_bench(struct ocssd_sblk *sblk);

static void
usage(void)
{
	printf("usage: hn:t:i:l:c:w\n");
}

char	*bt_name = "Nvme0n1";
int	be_type = 1;
char	*spdk_conf_file;
int	sblk_index;
int	lun_range[2];
int	chunk_idx = 1;
bool	op_write = false;

static void
get_options(int argc, char **argv)
{
	int ch;
	char *sep;
	opterr=0;

	while((ch=getopt(argc,argv,"hn:t:c:i:l:k:w"))!=-1)
	{
		switch(ch)
		{
		case 'n':
			bt_name = strdup(optarg);
			printf("bt name:\t%s\n",bt_name);
			break;
		case 't':
			be_type = (int)strtol(optarg, NULL, 0);
			printf("backend type:\t%d\n",be_type);
			break;
		case 'c':
			spdk_conf_file = strdup(optarg);
			printf("spdk_conf_file:\t%s\n",spdk_conf_file);
			break;
		case 'i':
			sblk_index = (int)strtol(optarg, NULL, 0);
			printf("sblk_index:\t%d\n",sblk_index);
			break;
		case 'l':
			lun_range[0] = (int)strtol(optarg, &sep, 0);
			lun_range[1] = (int)strtol(sep + 1, NULL, 0);
			printf("lun range:\t%d - %d\n", lun_range[0], lun_range[1]);
			break;
		case 'k':
			chunk_idx = (int)strtol(optarg, NULL, 0);
			printf("chunk_idx:\t%d\n",chunk_idx);
			break;
		case 'w':
			op_write = true;
			printf("Operation:\t%s\n", op_write ? "Write" : "Read");
			break;
		default:
			printf("Invalid option:%c\n",ch);
		case 'h':
			usage();
			exit(1);
		}
	}
}

int main(int argc, char **argv)
{
	int rc;
	struct ocssd_dev *dev;
	struct spdk_bdev_target *bt = NULL;
	struct ocssd_sblk *sblk;

	get_options(argc, argv);

	rc = spdk_env_setup(spdk_conf_file);
	if (rc) {
		fprintf(stderr, "Failed to set up spdk env\n");
		exit(1);
	}

	rc = spdk_bt_open(bt_name, &bt);
	if (rc) {
		fprintf(stderr, "Failed to open bt\n");
		exit(1);
	}

	rc = ocssd_dev_init(bt, &dev);
	if (rc) {
		fprintf(stderr, "Failed to init ocssd dev\n");
		exit(1);
	}

	rc = ocssd_sblk_construct(dev, &sblk, lun_range[0], lun_range[1], chunk_idx);
	if (rc) {
		fprintf(stderr, "Failed to construct ocssd dev sblk\n");
		exit(1);
	}

	rc = ocssd_sblk_check(sblk);
	if (rc) {
		fprintf(stderr, "Failed to check ocssd dev sblk\n");
		exit(1);
	}

	rc = ocssd_sblk_align(sblk);
	if (rc) {
		fprintf(stderr, "Failed to align ocssd dev sblk\n");
		exit(1);
	}

	rc = ocssd_sblk_bench(sblk);
	if (rc) {
		fprintf(stderr, "Failed to bench ocssd dev sblk\n");
		exit(1);
	}



	spdk_bt_close(bt);
	spdk_env_unset();

	return 0;
}

static void
print_ocssd_chunk_info(struct spdk_ocssd_chunk_information_entry *chk_info, int chk_num)
{
	int i;
	char *cs_str, *ct_str;

	printf("OCSSD Chunk Info Glance\n");
	printf("======================\n");

	for (i = 0; i < chk_num; i++) {
		cs_str = chk_info[i].cs.free ? "Free" :
			 chk_info[i].cs.closed ? "Closed" :
			 chk_info[i].cs.open ? "Open" :
			 chk_info[i].cs.offline ? "Offline" : "Unknown";
		ct_str = chk_info[i].ct.seq_write ? "Sequential Write" :
			 chk_info[i].ct.rnd_write ? "Random Write" : "Unknown";

		printf("------------\n");
		printf("Chunk index:                    %d\n", i);
		printf("Chunk state:                    %s(0x%x)\n", cs_str, *(uint8_t *) & (chk_info[i].cs));
		printf("Chunk type (write mode):        %s\n", ct_str);
		printf("Chunk type (size_deviate):      %s\n", chk_info[i].ct.size_deviate ? "Yes" : "No");
		printf("Wear-level Index:               %d\n", chk_info[i].wli);
		printf("Starting LBA:                   0x%lx\n", chk_info[i].slba);
		printf("Number of blocks in chunk:      %ld\n", chk_info[i].cnlb);
		printf("Write Pointer:                  0x%lx\n", chk_info[i].wp);
	}
}

static void
print_ocssd_geometry(struct spdk_ocssd_geometry_data *geometry_data)
{
	printf("Namespace OCSSD Geometry\n");
	printf("=======================\n");

	if (geometry_data->mjr < 2) {
		printf("Open-Channel Spec version is less than 2.0\n");
		printf("OC version:             maj:%d\n", geometry_data->mjr);
		return;
	}

	printf("OC version:                     maj:%d min:%d\n", geometry_data->mjr, geometry_data->mnr);
	printf("LBA format:\n");
	printf("  Group bits:                   %d\n", geometry_data->lbaf.grp_len);
	printf("  PU bits:                      %d\n", geometry_data->lbaf.pu_len);
	printf("  Chunk bits:                   %d\n", geometry_data->lbaf.chk_len);
	printf("  Logical block bits:           %d\n", geometry_data->lbaf.lbk_len);

	printf("Media and Controller Capabilities:\n");
	printf("  Namespace supports Vector Chunk Copy:                 %s\n",
	       geometry_data->mccap.vec_chk_cpy ? "Supported" : "Not Supported");
	printf("  Namespace supports multiple resets a free chunk:      %s\n",
	       geometry_data->mccap.multi_reset ? "Supported" : "Not Supported");

	printf("Wear-level Index Delta Threshold:                       %d\n", geometry_data->wit);
	printf("Groups (channels):              %d\n", geometry_data->num_grp);
	printf("PUs (LUNs) per group:           %d\n", geometry_data->num_pu);
	printf("Chunks per LUN:                 %d\n", geometry_data->num_chk);
	printf("Logical blks per chunk:         %d\n", geometry_data->clba);
	printf("MIN write size:                 %d\n", geometry_data->ws_min);
	printf("OPT write size:                 %d\n", geometry_data->ws_opt);
	printf("Cache min write size:           %d\n", geometry_data->mw_cunits);
	printf("Max open chunks:                %d\n", geometry_data->maxoc);
	printf("Max open chunks per PU:         %d\n", geometry_data->maxocpu);
	printf("\n");
}

static int
ocssd_dev_geo_idty(struct spdk_bdev_target *bt, struct spdk_ocssd_geometry_data *geo_data, int nsid)
{
	struct spdk_nvme_cmd cmd = { 0 };
	struct spdk_bdev_ret ret;
	char *pin_buffer;
	int rc;

	cmd.opc = SPDK_OCSSD_OPC_GEOMETRY;
	cmd.nsid = nsid;

	pin_buffer = spdk_dma_malloc(sizeof(*geo_data), 0x1000, NULL);
	if (pin_buffer == NULL) {
		return -ENOMEM;
	}

	rc = spdk_bdev_aio_req_admin_passthru_sync(bt,
			&cmd, pin_buffer, 0x1000,
			&ret);
	if (rc == 0) {
		memcpy(geo_data, pin_buffer, sizeof(*geo_data));
	}
	spdk_dma_free(pin_buffer);

	if (rc || ret.status != 0) {
		return -1;
	}

	return 0;
}

static int
ocssd_dev_ns_idty(struct spdk_bdev_target *bt, struct spdk_nvme_ns_data *ns_data, int nsid)
{
	struct spdk_nvme_cmd cmd = { 0 };
	struct spdk_bdev_ret ret;
	char *pin_buffer;
	int rc;

	cmd.opc = 0x06; // identify
	cmd.nsid = nsid;

	pin_buffer = spdk_dma_malloc(sizeof(*ns_data), 0x1000, NULL);
	if (pin_buffer == NULL) {
		return -ENOMEM;
	}

	rc = spdk_bdev_aio_req_admin_passthru_sync(bt,
			&cmd, pin_buffer, 0x1000,
			&ret);
	if (rc == 0) {
		memcpy(ns_data, pin_buffer, sizeof(*ns_data));
	}
	spdk_dma_free(pin_buffer);

	if (rc || ret.status != 0) {
		return -1;
	}

	return 0;
}

static int
ocssd_dev_init(struct spdk_bdev_target *bt, struct ocssd_dev **_dev)
{
	struct ocssd_dev *dev;
	int rc;

	dev = malloc(sizeof(*dev));
	if (!dev) {
		return -ENOMEM;
	}
	dev->nsid = 1;
	dev->bt = bt;

	rc = ocssd_dev_ns_idty(bt, &dev->ns_data, dev->nsid);
	if (rc) {
		fprintf(stderr, "Failed to idty ns\n");
		free(dev);
		return rc;
	}

	rc = ocssd_dev_geo_idty(bt, &dev->geo_data, dev->nsid);
	if (rc) {
		fprintf(stderr, "Failed to idty geo\n");
		free(dev);
		return rc;
	}
	print_ocssd_geometry(&dev->geo_data);

	dev->lba_num.grp = dev->geo_data.num_grp;
	dev->lba_num.pu = dev->geo_data.num_pu;
	dev->lba_num.chunk = dev->geo_data.num_chk;
	dev->lba_num.sector = dev->geo_data.clba;
	dev->lba_num.sbytes = 1 << dev->ns_data.lbaf[dev->ns_data.flbas.format & 0xf].lbads;
	dev->lba_num.sbytes_oob = dev->ns_data.lbaf[dev->ns_data.flbas.format & 0xf].ms;

	dev->lba_off.sector = 0;
	dev->lba_off.chunk = dev->geo_data.lbaf.lbk_len;
	dev->lba_off.pu = dev->lba_off.chunk + dev->geo_data.lbaf.chk_len;
	dev->lba_off.grp = dev->lba_off.pu + dev->geo_data.lbaf.pu_len;

	dev->lba_mask.sector = 0;

	*_dev = dev;
	return 0;
}

static int
ocssd_dev_chunk_idty(struct spdk_bdev_target *bt,
		uint64_t chunk_info_offset, int nchunks,
		struct spdk_ocssd_chunk_information_entry *chks_info, int nsid)
{
	struct spdk_nvme_cmd cmd = { 0 };
	struct spdk_bdev_ret ret;
	char *pin_buffer;
	int rc;
	uint32_t nbytes_loop, numd;
	uint16_t numdu, numdl;
	uint32_t i;

	nbytes_loop = 0x1000;
	numd = (nbytes_loop >> 2) - 1;
	numdu = numd >> 16;
	numdl = numd & 0xffff;

	cmd.opc = SPDK_NVME_OPC_GET_LOG_PAGE;
	cmd.nsid = nsid;
	cmd.cdw10 = 0xC4 | (numdl << 16);
	cmd.cdw11 = numdu;

	pin_buffer = spdk_dma_malloc(nbytes_loop, 0x1000, NULL);
	if (pin_buffer == NULL) {
		return -ENOMEM;
	}

	for (i = 0; i < nchunks * sizeof(*chks_info); i += nbytes_loop) {
		cmd.cdw12 = chunk_info_offset + i;
		cmd.cdw13 = (chunk_info_offset + i) >> 32;

		/* Check whether nbytes need to be updated if less data is remained. */
		if (nbytes_loop > nchunks * sizeof(*chks_info) - i) {
			nbytes_loop = nchunks * sizeof(*chks_info) - i;
			numd = (nbytes_loop >> 2) - 1;
			numdu = numd >> 16;
			numdl = numd & 0xffff;
			cmd.cdw10 = 0xC4 | (numdl << 16);
			cmd.cdw11 = numdu;
		}

		rc = spdk_bdev_aio_req_admin_passthru_sync(bt,
				&cmd, pin_buffer, nbytes_loop,
				&ret);
		if (rc != 0) {
			break;
		}

		memcpy((char *)chks_info + i, pin_buffer, nbytes_loop);
	}

	spdk_dma_free(pin_buffer);

	if (rc || ret.status != 0) {
		return -1;
	}

	return 0;
}

static int
ocssd_sblk_construct(struct ocssd_dev *dev, struct ocssd_sblk **_sblk,
		int lun_start, int lun_end, int chunk_idx)
{
	struct ocssd_sblk *sblk;
	int rc, i;
	int lun_index;

	sblk = malloc(sizeof(*sblk));
	if (!sblk) {
		return -ENOMEM;
	}

	sblk->dev = dev;
	sblk->nblk = lun_end - lun_start + 1;

	for (lun_index = lun_start, i = 0; i < sblk->nblk; lun_index++, i++) {
		uint64_t chunk_info_offset;
		struct spdk_ocssd_chunk_information_entry chk_info;

		sblk->blks[i].grp = lun_index % dev->lba_num.grp;
		sblk->blks[i].pu = lun_index / dev->lba_num.grp;
		sblk->blks[i].chunk = chunk_idx;
		chunk_info_offset = ocssd_dev_gen_chunk_info_offset(&dev->lba_num,
				sblk->blks[i].grp, sblk->blks[i].pu, sblk->blks[i].chunk);

		rc = ocssd_dev_chunk_idty(dev->bt, chunk_info_offset, 1, &chk_info, dev->nsid);
		if (rc) {
			free(sblk);
			return -1;
		}

		sblk->blks[i].ci = chk_info;
	}


	sblk->clba = dev->lba_num.sector;
	*_sblk = sblk;
	return 0;
}

/* check for offline chunk */
static int
ocssd_sblk_check(struct ocssd_sblk *sblk)
{
	int i;

	for (i = 0; i < sblk->nblk; i++) {
		if (sblk->blks[i].ci.cs.offline) {
			print_ocssd_chunk_info(&sblk->blks[i].ci, 1);
			fprintf(stderr, "Offline Chunk in grp %d, pu %d, chunk 0x%x\n",
					sblk->blks[i].grp, sblk->blks[i].pu, sblk->blks[i].chunk);
			return -1;
		}
	}
	sblk->checked = true;

	return 0;
}


static int
ocssd_chunkreset_intel34(struct spdk_bdev_target *bt,
		struct spdk_bdev_ret *nvm_ret,
		uint32_t nsid, uint64_t ppa,
		bool pined_buf)
{
	int rc;
	struct spdk_nvme_cmd cmd = { 0 };

	cmd.opc = SPDK_OCSSD_OPC_VECTOR_RESET;
	cmd.nsid = nsid;

	cmd.cdw10 = 0x0; // physical reset to free state

	*(uint64_t *)&cmd.cdw14 = ppa;

	rc = spdk_bdev_aio_req_io_passthru_sync(bt,
			(struct spdk_nvme_cmd *)&cmd, NULL, 0, NULL, 0,
			nvm_ret);

	return rc;
}

static int
ocssd_rw_intel34(struct spdk_bdev_target *bt,
		struct spdk_bdev_ret *nvm_ret,
		uint32_t nsid, uint64_t ppa, uint64_t lba,
		void *data, uint32_t data_len, void *meta, uint32_t md_len, uint16_t flags, bool read,
		bool pined_buf)
{
	int rc;
	void *pin_buffer = data;
	struct spdk_nvme_cmd cmd = { 0 };

	if (read) {
		cmd.opc = 0x02;
	} else {
		cmd.opc = 0x01;
	}
	cmd.nsid = nsid;

	*(uint64_t *)&cmd.cdw10 = lba;
	*(uint64_t *)&cmd.cdw14 = ppa;

	if (!pined_buf) {
		pin_buffer = spdk_dma_malloc(data_len, 4096, NULL);
		if (!pin_buffer) {
			return -1;
		}
		if (!read) {
			memcpy(pin_buffer, data, data_len);
		}
	}

	rc = spdk_bdev_aio_req_io_passthru_sync(bt,
			(struct spdk_nvme_cmd *)&cmd, pin_buffer, data_len, meta, md_len,
			nvm_ret);
	if (!pined_buf) {
		if (!rc && read) {
			memcpy(data, pin_buffer,  data_len);
		}
		spdk_dma_free(pin_buffer);
	}

	return rc;
}

static int
ocssd_sblk_align(struct ocssd_sblk *sblk)
{
	int i, rc = 0;
	uint64_t max_wp = 0;
	struct spdk_bdev_ret ret;

	for (i = 0; i < sblk->nblk; i++) {
		/* For POC dev, reset private vacant state to free*/
		if (sblk->blks[i].ci.cs.reserved == 1) {
			printf("reset vacant chunk in grp %d, pu %d, chunk 0x%x\n",
					sblk->blks[i].grp, sblk->blks[i].pu, sblk->blks[i].chunk);

			rc = ocssd_chunkreset_intel34(sblk->dev->bt,
					&ret,
					sblk->dev->nsid, sblk->blks[i].ci.slba,
					0);
			if (rc) {
				return -1;
			}
		}

		max_wp = spdk_max(max_wp, sblk->blks[i].ci.wp);
	}

	printf("sblk max_wp is 0x%lx\n", max_wp);
	/* coalescing */
	max_wp = (max_wp + 0x20 -1) / 0x20 * 0x20;
	printf("sblk max_wp aligned is 0x%lx\n", max_wp);

	uint32_t data_len = sblk->dev->lba_num.sbytes;
	uint64_t ppa;
	char *pin_buffer = spdk_dma_malloc(data_len, 4096, NULL);
	if (!pin_buffer) {
		return -ENOMEM;
	}

	for (i = 0; i < sblk->nblk; i++) {
		if (sblk->blks[i].ci.wp >= max_wp) {
			continue;
		}

		for (uint64_t j = sblk->blks[i].ci.wp; j < max_wp; j++) {
			ppa = sblk->blks[i].ci.slba + j;
			rc = ocssd_rw_intel34(sblk->dev->bt,
					&ret, sblk->dev->nsid, ppa, ppa,
					pin_buffer, data_len, NULL, 0, 0, 0,
					1);
			if (rc) {
				goto out;
			}
		}

		printf("Aligned at %lu in grp %d, pu %d, chunk 0x%x\n", max_wp,
				sblk->blks[i].grp, sblk->blks[i].pu, sblk->blks[i].chunk);
	}

	sblk->aligned = true;
	sblk->sector_offset = max_wp;
out:
	spdk_dma_free(pin_buffer);

	return rc;
}



static int
ocssd_rw_intel34_batch(struct spdk_bdev_target *bt,
		uint32_t nsid, uint64_t start_ppa, int n,
		void *data, uint32_t data_len, void *meta, uint32_t md_len, uint16_t flags, bool read)
{
	struct spdk_bdev_aio_ctx ctx[1] = {};
	struct spdk_bdev_aio_req reqs[0x100] = {};
	struct spdk_bdev_aio_req *reqs_p[0x100] = {};
	struct spdk_nvme_cmd cmds[0x100] = {};
	int rc, i;

	rc = spdk_bdev_aio_ctx_setup(ctx, bt);

	for (i = 0; i < n; i++) {
		if (read) {
			cmds[i].opc = 0x02;
		} else {
			cmds[i].opc = 0x01;
		}
		cmds[i].nsid = nsid;

		*(uint64_t *)&cmds[i].cdw10 = 0x12345678;
		*(uint64_t *)&cmds[i].cdw14 = start_ppa + i;

		spdk_bdev_aio_req_prep_io_passthru(&reqs[i], &cmds[i], data + data_len/n * i, data_len/n,
				meta + md_len/n * i, md_len/n);

		reqs_p[i] = &reqs[i];
	}

	rc = spdk_bdev_aio_ctx_submit(ctx, n, reqs_p);
	if (rc) {
		fprintf(stderr, "Failed to submit ctx's reqs. rc = %d\n", rc);
	} else {

		rc = spdk_bdev_aio_ctx_get_reqs(ctx,
				n, n, reqs_p, NULL);
		if (rc) {
			fprintf(stderr,  "Failed to get ctx's reqs. rc = %d\n", rc);
		}
	}

	spdk_bdev_aio_ctx_destroy(ctx, false);

	if (rc) {
		return rc;
	}

	for (i = 0; i < n; i++) {
		if (reqs_p[i]->req_rc || reqs_p[i]->ret.status) {
			return -1;
		}
	}

	return 0;
}

static int
ocssd_sblk_bench(struct ocssd_sblk *sblk)
{
	int i, rc = 0;
	int n = 0x40;
	uint64_t start_ppa;

	uint32_t data_len = sblk->dev->lba_num.sbytes * n;
	char *pin_buffer = spdk_dma_malloc(data_len, 4096, NULL);
	if (!pin_buffer) {
		return -ENOMEM;
	}

	uint64_t ticks_user[2] = {0};
	int user_n = 0;
	uint64_t hz = spdk_get_ticks_hz();

	if (!op_write) {
		sblk->sector_offset = 0x8000;
	}

	ticks_user[user_n++] =  spdk_get_ticks();
	for (uint64_t j = sblk->sector_offset; j < sblk->clba; j+=n) {
		for (i = 0; i < sblk->nblk; i++) {
			start_ppa = sblk->blks[i].ci.slba + j;
			rc = ocssd_rw_intel34_batch(sblk->dev->bt,
					sblk->dev->nsid, start_ppa, n,
					pin_buffer, data_len, NULL, 0, 0, !op_write);
			if (rc) {
				printf("start ppa is 0x%lx, batch number is %d\n", start_ppa, n);
				goto out;
			}
		}
	}

	ticks_user[user_n++] =  spdk_get_ticks();
	for (int i = 0; i < user_n; i++) {
		uint64_t ticks = ticks_user[i];
		printf("user index %d, micro second is %lu, ticks is %lu, hz is %lu\n", i, ticks/(hz/1000/1000), ticks, hz);
	}
	printf("Total time cost is %lu msec\n", (ticks_user[1] - ticks_user[0]) /(hz/1000));
	printf("Wrote out at %u from %u between %d chunks\n", sblk->clba, sblk->sector_offset, sblk->nblk);

out:
	spdk_dma_free(pin_buffer);

	return rc;
}
