#ifndef SPDKSUMMERSCHOOL_MERGE_REQUESTS_H
#define SPDKSUMMERSCHOOL_MERGE_REQUESTS_H

#define MAX_BLOCKS_FOR_REQUEST 4
#include "spdk/tree.h"

/**
 * Clearing the tree after merging the request
 * and freeing the memory of each malloc_write_request
 * in the tree.
 */
static void
clear_tree();

struct malloc_write_request {
    uint32_t addr;
    RB_ENTRY(malloc_write_request) link;
    struct spdk_bdev_io *bdev_io;
    struct malloc_channel *mch;
};

/**
 * Comparison of two request addresses for RB_TREE to work correctly
 */
static int
addr_cmp(struct malloc_write_request *c1 , struct malloc_write_request *c2);

RB_HEAD(malloc_addr_tree, malloc_write_request);
RB_GENERATE_STATIC(malloc_addr_tree, malloc_write_request, link, addr_cmp);

struct malloc_request_tree {
    struct malloc_addr_tree tree;
    uint64_t size;
} addr_tree;

/**
 * The function of intercepting requests and adding them to the tree.
 * It also checks the size of the tree and calls the malloc_merge_request
 * if the size of the tree is equal to MAX_BLOCKS_FOR_REQUEST.
 * Return 0 - the merge function was called and bdev_io was substituted for the merged one.
 * Return 1 - intercepting the write request and adding it to the tree.
 */
static int
interception_malloc_write_request(struct spdk_bdev_io *bdev_io, struct malloc_channel *mch);

#endif //SPDKSUMMERSCHOOL_MERGE_REQUESTS_H
