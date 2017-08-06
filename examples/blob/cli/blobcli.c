/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/log.h"
static const char* program_name = "blobcli";
static const char* program_conf = "blobcli.conf";
static const char* ver = "0.0";

static void
usage(char* msg)
{
	if (msg) {
		printf("%s", msg);
	}
	printf("\nversion %s\n", ver);
	printf("Usage: %s [options] command\n", program_name);
	printf("\n");
	printf("%s is a command line tool for interacting with blobstore\n",
	       program_name);
	printf("on the underlying device specified in the conf file passed\n");
	printf("in as a command line option.\n");
	printf("\nOptions include:\n");
	printf("\t-c conf_file - the SPDK config file\n");

	printf("\nCommands include:\n");
	printf("\tinit - initialized a blobstore\n");
	printf("\n");
}

int
main(int argc, char **argv)
{
	struct spdk_app_opts opts = {};
	const char *config_file = NULL;
	int rc = 0;
	int op;

	if (argc == 1) {
		usage("ERROR: No command specified\n");
		exit(1);
	}

	while ((op = getopt(argc, argv, "c:i:")) != -1) {
		switch (op) {
			case 'c':
				config_file = optarg;
				break;
			case 'i':
				break;
			default:
				usage("ERROR: Invalid option\n");
				exit(1);
		}
	}

	/* if they don't supply a conf name, use the default */
	if (!config_file) {
		config_file = program_conf;
	}

	/* Set default values in opts struct along with name and conf file. */
	spdk_app_opts_init(&opts);
	opts.name = "blobcli";
	opts.config_file = config_file;


	/* Gracefully close out all of the SPDK subsystems. */
	spdk_app_fini();
	return rc;
}