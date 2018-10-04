#ifndef __INTERNAL_OCSSD_DEV_H
#define __INTERNAL_OCSSD_DEV_H

#include "spdk/nvme_ocssd_spec.h"

#define OCSSD_DEV_NAME_LEN 32
#define OCSSD_DEV_MAX_LUNS 128

struct ocssd_dev_lba_num {
	uint64_t grp;
	uint64_t pu;
	uint64_t chunk;
	uint64_t sector;

	uint64_t sbytes;		///< # Bytes per SECTOR
	uint64_t sbytes_oob;	///< # Bytes per SECTOR in OOB
};

struct ocssd_dev_lba_offset {
	uint64_t grp;
	uint64_t pu;
	uint64_t chunk;
	uint64_t sector;
};

struct ocssd_dev_lba_mask {
	uint64_t grp;
	uint64_t pu;
	uint64_t chunk;
	uint64_t sector;
};

struct ocssd_dev {
	struct spdk_bdev_target *bt;
	char name[OCSSD_DEV_NAME_LEN];	///< Device name e.g. "nvme0n1"
	int nsid;			///< NVME namespace identifier
	struct spdk_ocssd_geometry_data geo_data;
	struct spdk_nvme_ns_data	ns_data;
	struct ocssd_dev_lba_offset lba_off;	///< Sector address format offset
	struct ocssd_dev_lba_mask lba_mask;	///< Sector address format mask
	struct ocssd_dev_lba_num lba_num;

//	struct nvm_spec_lbaf lbaf;	///< Logical device address format
//	uint8_t verid;			///< Open-Channel SSD version identifier
//	struct nvm_geo geo;		///< Device geometry
//	uint64_t ssw;			///< Bit-width for LBA fmt conversion
//	uint32_t mccap;			///< Media-controller capabilities
//	int pmode;			///< Default plane-mode I/O
//	int erase_naddrs_max;		///< Maximum # of cmd-addrs. for erase
//	int read_naddrs_max;		///< Maximum # of cmd-addrs. for read
//	int write_naddrs_max;		///< Maximum # of cmd-addrs. for write
//	int bbts_cached;		///< Whether to cache bbts
//	size_t nbbts;			///< Number of entries in cache
//	struct nvm_bbt **bbts;		///< Cache of bad-block-tables
//	enum nvm_meta_mode meta_mode;	///< Flag to indicate the how meta is w
//	struct nvm_be *be;		///< Backend interface
//	int quirks;			///< Mask representing known quirks
//	void *be_state;			///< Backend state
};

struct ocssd_blk {
	int grp;
	int pu;
	int chunk;

	struct spdk_ocssd_chunk_information_entry ci;
};

struct ocssd_sblk {
	struct ocssd_dev *dev;
	int 	nblk;
	struct ocssd_blk blks[OCSSD_DEV_MAX_LUNS];

	bool checked;
	bool aligned;
	int sector_offset;

	uint32_t clba;
};

static inline uint64_t
ocssd_dev_gen_chunk_info_offset(struct ocssd_dev_lba_num *lba_num,
		int grp, int pu, int chunk)
{
	uint64_t idx = 0;

	idx = grp * lba_num->pu * lba_num->chunk;
	idx += pu * lba_num->chunk;
	idx += chunk;

	return idx * sizeof(struct spdk_ocssd_chunk_information_entry);
}

#endif /* __INTERNAL_OCSSD_DEV_H */
