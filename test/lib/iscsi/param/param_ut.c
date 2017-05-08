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

#include "spdk/scsi.h"

#include "spdk_cunit.h"

#include "../common.c"
#include "iscsi/param.c"

struct spdk_iscsi_globals g_spdk_iscsi;

struct spdk_iscsi_tgt_node *
spdk_iscsi_find_tgt_node(const char *target_name)
{
	return NULL;
}

int
spdk_iscsi_tgt_node_access(struct spdk_iscsi_conn *conn,
			   struct spdk_iscsi_tgt_node *target,
			   const char *iqn, const char *addr)
{
	return 0;
}

int
spdk_iscsi_send_tgts(struct spdk_iscsi_conn *conn, const char *iiqn,
		     const char *iaddr,
		     const char *tiqn, uint8_t *data, int alloc_len, int data_len)
{
	return 0;
}

static void
burst_length_param_negotation(int FirstBurstLength, int MaxBurstLength,
			      int initialR2T)
{
	struct spdk_iscsi_sess sess;
	struct spdk_iscsi_conn conn;
	struct iscsi_param *params;
	char data[8192];
	int rc;
	int total, len;

	total = 0;
	params = NULL;

	memset(&sess, 0, sizeof(sess));
	memset(&conn, 0, sizeof(conn));
	memset(data, 0, 8192);

	sess.ExpCmdSN = 0;
	sess.MaxCmdSN = 64;
	sess.session_type = SESSION_TYPE_NORMAL;
	sess.params = NULL;
	sess.MaxBurstLength = 65536;
	sess.InitialR2T = 1;
	sess.FirstBurstLength = SPDK_ISCSI_FIRST_BURST_LENGTH;
	sess.MaxOutstandingR2T = 1;

	/* set default params */
	rc = spdk_iscsi_sess_params_init(&sess.params);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_param_set_int(sess.params, "FirstBurstLength",
				      sess.FirstBurstLength);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_param_set_int(sess.params, "MaxBurstLength",
				      sess.MaxBurstLength);
	CU_ASSERT(rc == 0);

	rc = spdk_iscsi_param_set(sess.params, "InitialR2T",
				  sess.InitialR2T ? "Yes" : "No");
	CU_ASSERT(rc == 0);

	conn.full_feature = 1;
	conn.sess = &sess;
	conn.MaxRecvDataSegmentLength = 65536;

	rc = spdk_iscsi_conn_params_init(&conn.params);
	CU_ASSERT(rc == 0);

	/* construct the data */
	len = snprintf(data + total, 8192 - total, "%s=%d",
		       "FirstBurstLength", FirstBurstLength);
	total += len + 1;

	len = snprintf(data + total, 8192 - total, "%s=%d",
		       "MaxBurstLength", MaxBurstLength);
	total += len + 1;

	len = snprintf(data + total, 8192 - total, "%s=%d",
		       "InitialR2T", initialR2T);
	total += len + 1;

	/* add one extra NUL byte at the end to match real iSCSI params */
	total++;

	/* store incoming parameters */
	rc = spdk_iscsi_parse_params(&params, data, total, false, NULL);
	CU_ASSERT(rc == 0);

	/* negotiate parameters */
	rc = spdk_iscsi_negotiate_params(&conn, params,
					 data, 8192, rc);
	CU_ASSERT(rc > 0);

	rc = spdk_iscsi_copy_param2var(&conn);
	CU_ASSERT(rc == 0);
	CU_ASSERT(conn.sess->FirstBurstLength <= SPDK_ISCSI_FIRST_BURST_LENGTH);
	CU_ASSERT(conn.sess->FirstBurstLength <= conn.sess->MaxBurstLength);
	CU_ASSERT(conn.sess->MaxBurstLength <= SPDK_ISCSI_MAX_BURST_LENGTH);
	CU_ASSERT(conn.sess->MaxOutstandingR2T == 1);

	spdk_iscsi_param_free(sess.params);
	spdk_iscsi_param_free(conn.params);
	spdk_iscsi_param_free(params);
}

static void
param_negotiation_test(void)
{
	burst_length_param_negotation(8192, 16384, 0);
	burst_length_param_negotation(8192, 16384, 1);
	burst_length_param_negotation(8192, 1024, 1);
	burst_length_param_negotation(8192, 1024, 0);
	burst_length_param_negotation(512, 1024, 1);
	burst_length_param_negotation(512, 1024, 0);
}

static void
list_negotiation_test(void)
{
	int add_param_value = 0;
	struct iscsi_param param = {};
	char *new_val;
	char valid_list_buf[1024];
	char in_val_buf[1024];

#define TEST_LIST(valid_list, in_val, expected_result) \
	do { \
		snprintf(valid_list_buf, sizeof(valid_list_buf), "%s", valid_list); \
		snprintf(in_val_buf, sizeof(in_val_buf), "%s", in_val); \
		new_val = spdk_iscsi_negotiate_param_list(&add_param_value, &param, valid_list_buf, in_val_buf, NULL); \
		if (expected_result) { \
			SPDK_CU_ASSERT_FATAL(new_val != NULL); \
			CU_ASSERT_STRING_EQUAL(new_val, expected_result); \
		} \
	} while (0)

	TEST_LIST("None", "None", "None");
	TEST_LIST("CHAP,None", "None", "None");
	TEST_LIST("CHAP,None", "CHAP", "CHAP");
	TEST_LIST("KRB5,SRP,CHAP,None", "SRP,CHAP,None", "SRP");
	TEST_LIST("KRB5,SRP,CHAP,None", "CHAP,SRP,None", "CHAP");
	TEST_LIST("KRB5,SRP,CHAP,None", "SPKM1,SRP,CHAP,None", "SRP");
	TEST_LIST("KRB5,SRP,None", "CHAP,None", "None");
}

#define PARSE(strconst, partial_enabled, partial_text) \
	data = strconst; \
	len = sizeof(strconst); \
	rc = spdk_iscsi_parse_params(&params, data, len, partial_enabled, partial_text)

#define EXPECT_VAL(key, expected_value) \
	{ \
		const char *val = spdk_iscsi_param_get_val(params, key); \
		CU_ASSERT(val != NULL); \
		if (val != NULL) { \
			CU_ASSERT(strcmp(val, expected_value) == 0); \
		} \
	}

#define EXPECT_NULL(key) \
	CU_ASSERT(spdk_iscsi_param_get_val(params, key) == NULL)

static void
parse_valid_test(void)
{
	struct iscsi_param *params;
	int rc;

	params = NULL;
	char *data;
	int len;
	char *partial_parameter = NULL;

	/* simple test with a single key=value */
	PARSE("Abc=def\0", false, NULL);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("Abc", "def");

	/* multiple key=value pairs */
	PARSE("Aaa=bbbbbb\0Xyz=test\0", false, NULL);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("Aaa", "bbbbbb");
	EXPECT_VAL("Xyz", "test");

	/* value with embedded '=' */
	PARSE("A=b=c\0", false, NULL);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("A", "b=c");

	/* CHAP_C=AAAA.... with value length 8192 */
	len = strlen("CHAP_C=") + ISCSI_TEXT_MAX_VAL_LEN + 1/* null terminators */;
	data = malloc(len);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	memset(data, 'A', len);
	strcpy(data, "CHAP_C");
	data[6] = '=';
	data[len - 1] = '\0';
	rc = spdk_iscsi_parse_params(&params, data, len, false, NULL);
	CU_ASSERT(rc == 0);
	free(data);

	/* partial parameter: value is partial */
	PARSE("C=AAA\0D=B", true, &partial_parameter);
	SPDK_CU_ASSERT_FATAL(partial_parameter != NULL);
	CU_ASSERT_STRING_EQUAL(partial_parameter, "D=B");
	CU_ASSERT(rc == 0);
	EXPECT_VAL("C", "AAA");
	EXPECT_NULL("D");
	PARSE("XXXX\0E=UUUU\0", false, &partial_parameter);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("D", "BXXXX");
	EXPECT_VAL("E", "UUUU");
	CU_ASSERT_PTR_NULL(partial_parameter);

	/* partial parameter: key is partial */
	PARSE("IAMAFAK", true, &partial_parameter);
	CU_ASSERT_STRING_EQUAL(partial_parameter, "IAMAFAK");
	CU_ASSERT(rc == 0);
	EXPECT_NULL("IAMAFAK");
	PARSE("EDKEY=TTTT\0F=IIII", false, &partial_parameter);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("IAMAFAKEDKEY", "TTTT");
	EXPECT_VAL("F", "IIII");
	CU_ASSERT_PTR_NULL(partial_parameter);

	/* Second partial parameter is the only parameter */
	PARSE("OOOO", true, &partial_parameter);
	CU_ASSERT_STRING_EQUAL(partial_parameter, "OOOO");
	CU_ASSERT(rc == 0);
	EXPECT_NULL("OOOO");
	PARSE("LL=MMMM", false, &partial_parameter);
	CU_ASSERT(rc == 0);
	EXPECT_VAL("OOOOLL", "MMMM");
	CU_ASSERT_PTR_NULL(partial_parameter);

	spdk_iscsi_param_free(params);
}

static void
parse_invalid_test(void)
{
	struct iscsi_param *params;
	int rc;

	params = NULL;
	char *data;
	int len;

	/* key without '=' */
	PARSE("Abc\0", false, NULL);
	CU_ASSERT(rc != 0);
	EXPECT_NULL("Abc");

	/* multiple key=value pairs, one missing '=' */
	PARSE("Abc=def\0Xyz\0Www=test\0", false, NULL);
	CU_ASSERT(rc != 0);
	EXPECT_VAL("Abc", "def");
	EXPECT_NULL("Xyz");
	EXPECT_NULL("Www");

	/* empty key */
	PARSE("=abcdef", false, NULL);
	CU_ASSERT(rc != 0);
	EXPECT_NULL("");

	/* CHAP_C=AAAA.... with value length 8192 + 1 */
	len = strlen("CHAP_C=") + ISCSI_TEXT_MAX_VAL_LEN + 1 /* max value len + 1 */ +
	      1 /* null terminators */;
	data = malloc(len);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	memset(data, 'A', len);
	strcpy(data, "CHAP_C");
	data[6] = '=';
	data[len - 1] = '\0';
	rc = spdk_iscsi_parse_params(&params, data, len, false, NULL);
	free(data);
	CU_ASSERT(rc != 0);
	EXPECT_NULL("CHAP_C");

	/* Test simple value, length of value bigger than 255 */
	len = strlen("A=") + ISCSI_TEXT_MAX_SIMPLE_VAL_LEN + 1 /* max simple value len + 1 */ +
	      1 /* null terminators */;
	data = malloc(len);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	memset(data, 'A', len);
	data[1] = '=';
	data[len - 1] = '\0';
	rc = spdk_iscsi_parse_params(&params, data, len, false, NULL);
	free(data);
	CU_ASSERT(rc != 0);
	EXPECT_NULL("A");

	/* key length bigger than 63 */
	len = ISCSI_TEXT_MAX_KEY_LEN + 1 /* max key length + 1 */ + 1 /* = */ + 1 /* A */ +
	      1 /* null terminators */;
	data = malloc(len);
	SPDK_CU_ASSERT_FATAL(data != NULL);
	memset(data, 'A', len);
	data[64] = '=';
	data[len - 1] = '\0';
	rc = spdk_iscsi_parse_params(&params, data, len, false, NULL);
	free(data);
	CU_ASSERT(rc != 0);
	EXPECT_NULL("A");

	/* duplicated key */
	PARSE("B=BB", false, NULL);
	CU_ASSERT(rc == 0);
	PARSE("B=BBBB", false, NULL);
	CU_ASSERT(rc != 0);
	EXPECT_VAL("B", "BB");

	spdk_iscsi_param_free(params);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}

	suite = CU_add_suite("iscsi_suite", NULL, NULL);
	if (suite == NULL) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	if (
		CU_add_test(suite, "param negotiation test",
			    param_negotiation_test) == NULL ||
		CU_add_test(suite, "list negotation test",
			    list_negotiation_test) == NULL ||
		CU_add_test(suite, "parse valid test",
			    parse_valid_test) == NULL ||
		CU_add_test(suite, "parse invalid test",
			    parse_invalid_test) == NULL
	) {
		CU_cleanup_registry();
		return CU_get_error();
	}

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
