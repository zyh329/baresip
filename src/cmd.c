/**
 * @file src/cmd.c  Command Interface
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <ctype.h>
#include <string.h>
#include <re.h>
#include <baresip.h>
#include "core.h"


enum {
	REL = 0x00,
	ESC = 0x1b,
	DEL = 0x7f,
	LONG_PREFIX = '.'
};


struct cmds {
	struct le le;
	const struct cmd *cmdv;
	size_t cmdc;
};

struct cmd_ctx {
	struct mbuf *mb;
	const struct cmd *cmd;
	bool is_long;
};


static struct list cmdl;           /**< List of command blocks (struct cmds) */
static struct list longcmdl;


static void destructor(void *arg)
{
	struct cmds *cmds = arg;

	list_unlink(&cmds->le);
}


static void ctx_destructor(void *arg)
{
	struct cmd_ctx *ctx = arg;

	mem_deref(ctx->mb);
}


static int ctx_alloc(struct cmd_ctx **ctxp, const struct cmd *cmd)
{
	struct cmd_ctx *ctx;

	ctx = mem_zalloc(sizeof(*ctx), ctx_destructor);
	if (!ctx)
		return ENOMEM;

	ctx->mb = mbuf_alloc(32);
	if (!ctx->mb) {
		mem_deref(ctx);
		return ENOMEM;
	}

	ctx->cmd = cmd;

	*ctxp = ctx;

	return 0;
}


static struct cmds *cmds_find(const struct cmd *cmdv)
{
	struct le *le;

	if (!cmdv)
		return NULL;

	for (le = cmdl.head; le; le = le->next) {
		struct cmds *cmds = le->data;

		if (cmds->cmdv == cmdv)
			return cmds;
	}

	return NULL;
}


static const struct cmd *cmd_find_by_key(char key)
{
	struct le *le;

	for (le = cmdl.tail; le; le = le->prev) {

		struct cmds *cmds = le->data;
		size_t i;

		for (i=0; i<cmds->cmdc; i++) {

			const struct cmd *cmd = &cmds->cmdv[i];

			if (cmd->key == key && cmd->h)
				return cmd;
		}
	}

	return NULL;
}


static const char *cmd_name(char *buf, size_t sz, const struct cmd *cmd)
{
	switch (cmd->key) {

	case ' ':   return "SPACE";
	case '\n':  return "ENTER";
	case ESC:   return "ESC";
	}

	buf[0] = cmd->key;
	buf[1] = '\0';

	if (cmd->flags & CMD_PRM)
		strncat(buf, " ..", sz-1);

	return buf;
}


static int editor_input(struct mbuf *mb, char key,
			struct re_printf *pf, bool *del, bool is_long)
{
	int err = 0;

	switch (key) {

	case ESC:
		*del = true;
		return re_hprintf(pf, "\nCancel\n");

	case REL:
		break;

	case '\n':
		*del = true;
		return re_hprintf(pf, "\n");

	case '\b':
	case DEL:
		if (mb->pos > 0)
			mb->pos = mb->end = (mb->pos - 1);
		break;

	default:
		err = mbuf_write_u8(mb, key);
		break;
	}

	if (is_long)
		err |= re_hprintf(pf, "\r%b", mb->buf, mb->end);
	else
		err |= re_hprintf(pf, "\r> %32b", mb->buf, mb->end);

	return err;
}


static int cmd_report(const struct cmd *cmd, struct re_printf *pf,
		      struct mbuf *mb, bool compl, void *data)
{
	struct cmd_arg arg;
	int err;

	mb->pos = 0;
	err = mbuf_strdup(mb, &arg.prm, mb->end);
	if (err)
		return err;

	arg.key      = cmd->key;
	arg.complete = compl;
	arg.data     = data;

	err = cmd->h(pf, &arg);

	mem_deref(arg.prm);

	return err;
}


int cmd_process_long(const char *str, size_t len,
		     struct re_printf *pf_resp, void *data)
{
	struct cmd_arg arg;
	struct cmd_long *cmd_long;
	char *name = NULL, *prm = NULL;
	struct pl pl_name, pl_prm;
	int err;

	if (!str || !len)
		return EINVAL;

	memset(&arg, 0, sizeof(arg));

	err = re_regex(str, len, "[^ ]+[ ]*[~]*", &pl_name, NULL, &pl_prm);
	if (err) {
		re_printf("regex failed\n");
		return err;
	}

	pl_strdup(&name, &pl_name);
	if (pl_isset(&pl_prm))
		pl_strdup(&prm, &pl_prm);

	cmd_long = cmd_long_find(name);
	if (cmd_long) {

		arg.key      = 0;     // ? which key to use
		arg.name     = name;
		arg.prm      = prm;
		arg.complete = true;
		arg.data     = data;

		err = cmd_long->h(pf_resp, &arg);
	}
	else {
		err = re_hprintf(pf_resp, "command not found (%s)\n", name);
	}

	mem_deref(name);
	mem_deref(prm);

	return err;
}


static int cmd_process_edit(struct cmd_ctx **ctxp, char key,
			    struct re_printf *pf, void *data)
{
	struct cmd_ctx *ctx;
	bool compl = (key == '\n'), del = false;
	int err;

	if (!ctxp)
		return EINVAL;

	ctx = *ctxp;

	err = editor_input(ctx->mb, key, pf, &del, ctx->is_long);
	if (err)
		return err;

	if (ctx->is_long) {

		if (compl) {

			err = cmd_process_long((char *)ctx->mb->buf,
					       ctx->mb->end,
					       pf, NULL);
		}
	}
	else {
		if (compl ||
		    (ctx->cmd && ctx->cmd->flags & CMD_PROG))
			err = cmd_report(ctx->cmd, pf, ctx->mb, compl, data);
	}

	if (del)
		*ctxp = mem_deref(*ctxp);

	return err;
}


/**
 * Register commands
 *
 * @param cmdv Array of commands
 * @param cmdc Number of commands
 *
 * @return 0 if success, otherwise errorcode
 */
int cmd_register(const struct cmd *cmdv, size_t cmdc)
{
	struct cmds *cmds;

	if (!cmdv || !cmdc)
		return EINVAL;

	cmds = cmds_find(cmdv);
	if (cmds)
		return EALREADY;

	cmds = mem_zalloc(sizeof(*cmds), destructor);
	if (!cmds)
		return ENOMEM;

	cmds->cmdv = cmdv;
	cmds->cmdc = cmdc;

	list_append(&cmdl, &cmds->le, cmds);

	return 0;
}


static bool sort_handler(struct le *le1, struct le *le2, void *arg)
{
	struct cmd_long *cmd1 = le1->data, *cmd2 = le2->data;

	return str_casecmp(cmd2->name, cmd1->name) >= 0;
}


int  cmd_register_long(struct cmd_long *cmdv, size_t cmdc)
{
	size_t i;

	for (i=0; i<cmdc; i++) {

		struct cmd_long *cmd = &cmdv[i];

		if (cmd_long_find(cmd->name)) {
			warning("long command '%s' already registered\n",
				cmd->name);
			return EALREADY;
		}

		list_append(&longcmdl, &cmd->le, cmd);

		list_sort(&longcmdl, sort_handler, 0);
	}

	return 0;
}


/**
 * Unregister commands
 *
 * @param cmdv Array of commands
 */
void cmd_unregister(const struct cmd *cmdv)
{
	mem_deref(cmds_find(cmdv));
}


void cmd_unregister_long(struct cmd_long *cmdv, size_t cmdc)
{
	size_t i;

	if (!cmdv)
		return;

	for (i=0; i<cmdc; i++) {

		struct cmd_long *cmd = &cmdv[i];

		list_unlink(&cmd->le);
	}
}


struct cmd_long *cmd_long_find(const char *name)
{
	struct le *le;

	for (le = longcmdl.head; le; le = le->next) {

		struct cmd_long *cmd = le->data;

		if (0 == str_casecmp(name, cmd->name))
			return cmd;
	}

	return NULL;
}


/**
 * Process input characters to the command system
 *
 * @param ctxp Pointer to context for editor (optional)
 * @param key  Input character
 * @param pf   Print function
 * @param data Application data
 *
 * @return 0 if success, otherwise errorcode
 */
int cmd_process(struct cmd_ctx **ctxp, char key, struct re_printf *pf,
		void *data)
{
	const struct cmd *cmd;

	/* are we in edit-mode? */
	if (ctxp && *ctxp) {

		if (key == REL)
			return 0;

		return cmd_process_edit(ctxp, key, pf, data);
	}

	cmd = cmd_find_by_key(key);
	if (cmd) {
		struct cmd_arg arg;

		/* check for parameters */
		if (cmd->flags & CMD_PRM) {

			if (ctxp) {
				int err = ctx_alloc(ctxp, cmd);
				if (err)
					return err;
			}

			return cmd_process_edit(ctxp,
						isdigit(key) ? key : 0,
						pf, data);
		}

		arg.key      = key;
		arg.prm      = NULL;
		arg.complete = true;
		arg.data     = data;

		return cmd->h(pf, &arg);
	}
	else if (key == LONG_PREFIX) {

		int err;

		re_hprintf(pf, "\nPlease enter long command:\n");

		if (!ctxp) {
			warning("ctxp is reqired\n");
			return EINVAL;
		}

		err = ctx_alloc(ctxp, cmd);
		if (err)
			return err;

		(*ctxp)->is_long = true;

		return 0;
	}

	if (key == REL)
		return 0;

	return cmd_print(pf, NULL);
}


/**
 * Print a list of available commands
 *
 * @param pf     Print function
 * @param unused Unused variable
 *
 * @return 0 if success, otherwise errorcode
 */
int cmd_print(struct re_printf *pf, void *unused)
{
	size_t width = 5;
	char fmt[32], buf[8];
	int err = 0;
	int key;
	struct le *le;

	(void)unused;

	if (!pf)
		return EINVAL;

	(void)re_snprintf(fmt, sizeof(fmt), " %%-%zus   %%s\n", width);

	err |= re_hprintf(pf, "--- Help ---\n");

	/* print in alphabetical order */
	for (key = 1; key <= 0x80; key++) {

		const struct cmd *cmd = cmd_find_by_key(key);
		if (!cmd || !str_isset(cmd->desc))
			continue;

		err |= re_hprintf(pf, fmt, cmd_name(buf, sizeof(buf), cmd),
				  cmd->desc);

	}

	err |= re_hprintf(pf, "\n");

	/* Long commands */

	err |= re_hprintf(pf, "Long commands: (%u)\n", list_count(&longcmdl));

	for (le = longcmdl.head; le; le = le->next) {

		struct cmd_long *cmd = le->data;

		width = max(width, str_len(cmd->name));
	}

	(void)re_snprintf(fmt, sizeof(fmt), " %%c%%-%zus   %%s   %%s\n",
			  width);

	for (le = longcmdl.head; le; le = le->next) {

		struct cmd_long *cmd = le->data;

		err |= re_hprintf(pf, fmt, LONG_PREFIX, cmd->name,
				  (cmd->flags & CMD_PRM) ? ".." : "  ",
				  cmd->desc);
	}

	err |= re_hprintf(pf, "\n");

	return err;
}
