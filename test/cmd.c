/**
 * @file test/cmd.c  Baresip selftest -- cmd
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "test.h"


struct test {
	unsigned cmd_called;
};

static int cmd_test(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct test *test = carg->data;
	int err = 0;
	(void)pf;

	ASSERT_EQ('@', carg->key);
	ASSERT_TRUE(NULL == carg->prm);
	ASSERT_EQ(true, carg->complete);

	++test->cmd_called;

 out:
	return err;
}


static const struct cmd cmdv[] = {
	{'@',       0, "Test command",  cmd_test},
};


static int vprintf_null(const char *p, size_t size, void *arg)
{
	(void)p;
	(void)size;
	(void)arg;
	return 0;
}


static struct re_printf pf_null = {vprintf_null, 0};


int test_cmd(void)
{
	struct cmd_ctx *ctx = 0;
	struct test test;
	int err = 0;

	memset(&test, 0, sizeof(test));

	err = cmd_register(cmdv, ARRAY_SIZE(cmdv));
	ASSERT_EQ(0, err);

	/* issue a different command */
	err = cmd_process(&ctx, 'h', &pf_null, &test);
	ASSERT_EQ(0, err);
	ASSERT_EQ(0, test.cmd_called);

	/* issue our command, expect handler to be called */
	err = cmd_process(&ctx, '@', &pf_null, &test);
	ASSERT_EQ(0, err);
	ASSERT_EQ(1, test.cmd_called);

	cmd_unregister(cmdv);

	/* verify that context was not created */
	ASSERT_TRUE(NULL == ctx);

 out:
	return err;
}


static int long_handler(struct re_printf *pf, void *arg)
{
	struct cmd_arg *carg = arg;
	struct test *test = carg->data;
	int err = 0;
	(void)pf;

	ASSERT_STREQ("test", carg->name);
	ASSERT_STREQ("123", carg->prm);

	++test->cmd_called;

 out:
	return err;
}


static struct cmd_long longcmdv[] = {
	{ "test", 0, "Test Command", long_handler , LE_INIT},
};


int test_cmd_long(void)
{
	struct test test;
	struct cmd_long *cmd;
	int err;

	memset(&test, 0, sizeof(test));

	/* Verify that the command does not exist */
	cmd = cmd_long_find("test");
	ASSERT_TRUE(cmd == NULL);

	/* Register and verify command */
	err = cmd_register_long(longcmdv, ARRAY_SIZE(longcmdv));
	ASSERT_EQ(0, err);

	cmd = cmd_long_find("test");
	ASSERT_TRUE(cmd != NULL);

	/* Feed it some input data .. */

	err = cmd_process_long("test 123", 8, &pf_null, &test);
	ASSERT_EQ(0, err);

	ASSERT_EQ(1, test.cmd_called);

	/* Cleanup .. */

	cmd_unregister_long(longcmdv, ARRAY_SIZE(longcmdv));

	cmd = cmd_long_find("test");
	ASSERT_TRUE(cmd == NULL);

 out:
	return err;
}
