/*
 * module-eeprom.c - netlink implementation of module eeprom get command
 *
 * ethtool -m <dev>
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "../module-common.h"
#include "../qsfp.h"
#include "../cmis.h"
#include "../internal.h"
#include "../common.h"
#include "../list.h"
#include "netlink.h"
#include "parser.h"

#define ETH_I2C_ADDRESS_LOW	0x50
#define ETH_I2C_MAX_ADDRESS	0x7F

struct cmd_params {
	unsigned long present;
	u8 dump_hex;
	u8 dump_raw;
	u32 offset;
	u32 length;
	u32 page;
	u32 bank;
	u32 i2c_address;
};

enum {
	PARAM_OFFSET = 2,
	PARAM_LENGTH,
	PARAM_PAGE,
	PARAM_BANK,
	PARAM_I2C,
};

static const struct param_parser getmodule_params[] = {
	{
		.arg		= "hex",
		.handler	= nl_parse_u8bool,
		.dest_offset	= offsetof(struct cmd_params, dump_hex),
		.min_argc	= 1,
	},
	{
		.arg		= "raw",
		.handler	= nl_parse_u8bool,
		.dest_offset	= offsetof(struct cmd_params, dump_raw),
		.min_argc	= 1,
	},
	[PARAM_OFFSET] = {
		.arg		= "offset",
		.handler	= nl_parse_direct_u32,
		.dest_offset	= offsetof(struct cmd_params, offset),
		.min_argc	= 1,
	},
	[PARAM_LENGTH] = {
		.arg		= "length",
		.handler	= nl_parse_direct_u32,
		.dest_offset	= offsetof(struct cmd_params, length),
		.min_argc	= 1,
	},
	[PARAM_PAGE] = {
		.arg		= "page",
		.handler	= nl_parse_direct_u32,
		.dest_offset	= offsetof(struct cmd_params, page),
		.min_argc	= 1,
	},
	[PARAM_BANK] = {
		.arg		= "bank",
		.handler	= nl_parse_direct_u32,
		.dest_offset	= offsetof(struct cmd_params, bank),
		.min_argc	= 1,
	},
	[PARAM_I2C] = {
		.arg		= "i2c",
		.handler	= nl_parse_direct_u32,
		.dest_offset	= offsetof(struct cmd_params, i2c_address),
		.min_argc	= 1,
	},
	{}
};

static struct list_head eeprom_page_list = LIST_HEAD_INIT(eeprom_page_list);

struct eeprom_page_entry {
	struct list_head list;	/* Member of eeprom_page_list */
	void *data;
};

static int eeprom_page_list_add(void *data)
{
	struct eeprom_page_entry *entry;

	entry = malloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;

	entry->data = data;
	list_add(&entry->list, &eeprom_page_list);

	return 0;
}

static void eeprom_page_list_flush(void)
{
	struct eeprom_page_entry *entry;
	struct list_head *head, *next;

	list_for_each_safe(head, next, &eeprom_page_list) {
		entry = (struct eeprom_page_entry *) head;
		free(entry->data);
		list_del(head);
		free(entry);
	}
}

static int get_eeprom_page_reply_cb(const struct nlmsghdr *nlhdr, void *data)
{
	const struct nlattr *tb[ETHTOOL_A_MODULE_EEPROM_DATA + 1] = {};
	struct ethtool_module_eeprom *request = data;
	DECLARE_ATTR_TB_INFO(tb);
	u8 *eeprom_data;
	int ret;

	ret = mnl_attr_parse(nlhdr, GENL_HDRLEN, attr_cb, &tb_info);
	if (ret < 0)
		return ret;

	if (!tb[ETHTOOL_A_MODULE_EEPROM_DATA])
		return MNL_CB_ERROR;

	eeprom_data = mnl_attr_get_payload(tb[ETHTOOL_A_MODULE_EEPROM_DATA]);
	request->data = malloc(request->length);
	if (!request->data)
		return MNL_CB_ERROR;
	memcpy(request->data, eeprom_data, request->length);

	ret = eeprom_page_list_add(request->data);
	if (ret < 0)
		goto err_list_add;

	return MNL_CB_OK;

err_list_add:
	free(request->data);
	return MNL_CB_ERROR;
}

int nl_get_eeprom_page(struct cmd_context *ctx,
		       struct ethtool_module_eeprom *request)
{
	struct nl_context *nlctx = ctx->nlctx;
	struct nl_socket *nlsock;
	struct nl_msg_buff *msg;
	int ret;

	if (!request || request->i2c_address > ETH_I2C_MAX_ADDRESS)
		return -EINVAL;

	nlsock = nlctx->ethnl_socket;
	msg = &nlsock->msgbuff;

	ret = nlsock_prep_get_request(nlsock, ETHTOOL_MSG_MODULE_EEPROM_GET,
				      ETHTOOL_A_MODULE_EEPROM_HEADER, 0);
	if (ret < 0)
		return ret;

	if (ethnla_put_u32(msg, ETHTOOL_A_MODULE_EEPROM_LENGTH,
			   request->length) ||
	    ethnla_put_u32(msg, ETHTOOL_A_MODULE_EEPROM_OFFSET,
			   request->offset) ||
	    ethnla_put_u8(msg, ETHTOOL_A_MODULE_EEPROM_PAGE,
			  request->page) ||
	    ethnla_put_u8(msg, ETHTOOL_A_MODULE_EEPROM_BANK,
			  request->bank) ||
	    ethnla_put_u8(msg, ETHTOOL_A_MODULE_EEPROM_I2C_ADDRESS,
			  request->i2c_address))
		return -EMSGSIZE;

	ret = nlsock_sendmsg(nlsock, NULL);
	if (ret < 0)
		return ret;
	return nlsock_process_reply(nlsock, get_eeprom_page_reply_cb,
				    (void *)request);
}

static int eeprom_dump_hex(struct cmd_context *ctx)
{
	struct ethtool_module_eeprom request = {
		.length = 128,
		.i2c_address = ETH_I2C_ADDRESS_LOW,
	};
	int ret;

	ret = nl_get_eeprom_page(ctx, &request);
	if (ret < 0)
		return ret;

	dump_hex(stdout, request.data, request.length, request.offset);

	return 0;
}

static int eeprom_parse(struct cmd_context *ctx)
{
	struct ethtool_module_eeprom request = {
		.length = 1,
		.i2c_address = ETH_I2C_ADDRESS_LOW,
	};
	int ret;

	/* Fetch the SFF-8024 Identifier Value. For all supported standards, it
	 * is located at I2C address 0x50, byte 0. See section 4.1 in SFF-8024,
	 * revision 4.9.
	 */
	ret = nl_get_eeprom_page(ctx, &request);
	if (ret < 0)
		return ret;

	switch (request.data[0]) {
#ifdef ETHTOOL_ENABLE_PRETTY_DUMP
	case MODULE_ID_GBIC:
	case MODULE_ID_SOLDERED_MODULE:
	case MODULE_ID_SFP:
		return sff8079_show_all_nl(ctx);
	case MODULE_ID_QSFP:
	case MODULE_ID_QSFP28:
	case MODULE_ID_QSFP_PLUS:
		return sff8636_show_all_nl(ctx);
	case MODULE_ID_QSFP_DD:
	case MODULE_ID_OSFP:
	case MODULE_ID_DSFP:
	case MODULE_ID_QSFP_PLUS_CMIS:
	case MODULE_ID_SFP_DD_CMIS:
	case MODULE_ID_SFP_PLUS_CMIS:
		return cmis_show_all_nl(ctx);
#endif
	default:
		/* If we cannot recognize the memory map, default to dumping
		 * the first 128 bytes in hex.
		 */
		return eeprom_dump_hex(ctx);
	}
}

int nl_getmodule(struct cmd_context *ctx)
{
	struct cmd_params getmodule_cmd_params = {};
	struct ethtool_module_eeprom request = {0};
	struct nl_context *nlctx = ctx->nlctx;
	int ret;

	if (netlink_cmd_check(ctx, ETHTOOL_MSG_MODULE_EEPROM_GET, false))
		return -EOPNOTSUPP;

	nlctx->cmd = "-m";
	nlctx->argp = ctx->argp;
	nlctx->argc = ctx->argc;
	nlctx->devname = ctx->devname;
	ret = nl_parser(nlctx, getmodule_params, &getmodule_cmd_params, PARSER_GROUP_NONE, NULL);
	if (ret < 0)
		return ret;

	if (getmodule_cmd_params.dump_hex && getmodule_cmd_params.dump_raw) {
		fprintf(stderr, "Hex and raw dump cannot be specified together\n");
		return -EINVAL;
	}

	/* When complete hex/raw dump of the EEPROM is requested, fallback to
	 * ioctl. Netlink can only request specific pages.
	 */
	if ((getmodule_cmd_params.dump_hex || getmodule_cmd_params.dump_raw) &&
	    !(getmodule_cmd_params.present & (1 << PARAM_PAGE |
					      1 << PARAM_BANK |
					      1 << PARAM_I2C))) {
		nlctx->ioctl_fallback = true;
		return -EOPNOTSUPP;
	}

#ifdef ETHTOOL_ENABLE_PRETTY_DUMP
	if (getmodule_cmd_params.present & (1 << PARAM_PAGE |
					    1 << PARAM_BANK |
					    1 << PARAM_OFFSET |
					    1 << PARAM_LENGTH))
#endif
		getmodule_cmd_params.dump_hex = true;

	request.offset = getmodule_cmd_params.offset;
	request.length = getmodule_cmd_params.length ?: 128;
	request.page = getmodule_cmd_params.page;
	request.bank = getmodule_cmd_params.bank;
	request.i2c_address = getmodule_cmd_params.i2c_address ?: ETH_I2C_ADDRESS_LOW;

	if (request.page && !request.offset)
		request.offset = 128;

	if (getmodule_cmd_params.dump_hex || getmodule_cmd_params.dump_raw) {
		ret = nl_get_eeprom_page(ctx, &request);
		if (ret < 0)
			goto cleanup;

		if (getmodule_cmd_params.dump_raw)
			fwrite(request.data, 1, request.length, stdout);
		else
			dump_hex(stdout, request.data, request.length,
				 request.offset);
	} else {
		ret = eeprom_parse(ctx);
		if (ret < 0)
			goto cleanup;
	}

cleanup:
	eeprom_page_list_flush();
       return ret;
}

struct smodule_eeprom_params {
       unsigned long present;
       u32 offset;
       u32 length;
       u32 page;
       u32 bank;
       u32 i2c_address;
       u8 value;
};

enum {
       PARAM_S_OFFSET,
       PARAM_S_LENGTH,
       PARAM_S_PAGE,
       PARAM_S_BANK,
       PARAM_S_I2C,
       PARAM_S_VALUE,
};

static const struct param_parser setmodule_params[] = {
       [PARAM_S_OFFSET] = {
               .arg            = "offset",
               .handler        = nl_parse_direct_u32,
               .dest_offset    = offsetof(struct smodule_eeprom_params, offset),
               .min_argc       = 1,
       },
       [PARAM_S_LENGTH] = {
               .arg            = "length",
               .handler        = nl_parse_direct_u32,
               .dest_offset    = offsetof(struct smodule_eeprom_params, length),
               .min_argc       = 1,
       },
       [PARAM_S_PAGE] = {
               .arg            = "page",
               .handler        = nl_parse_direct_u32,
               .dest_offset    = offsetof(struct smodule_eeprom_params, page),
               .min_argc       = 1,
       },
       [PARAM_S_BANK] = {
               .arg            = "bank",
               .handler        = nl_parse_direct_u32,
               .dest_offset    = offsetof(struct smodule_eeprom_params, bank),
               .min_argc       = 1,
       },
       [PARAM_S_I2C] = {
               .arg            = "i2c",
               .handler        = nl_parse_direct_u32,
               .dest_offset    = offsetof(struct smodule_eeprom_params, i2c_address),
               .min_argc       = 1,
       },
       [PARAM_S_VALUE] = {
               .arg            = "value",
               .handler        = nl_parse_direct_u8,
               .dest_offset    = offsetof(struct smodule_eeprom_params, value),
               .min_argc       = 1,
       },
       {}
};

int nl_set_module_eeprom(struct cmd_context *ctx)
{
       struct nl_context *nlctx = ctx->nlctx;
       struct smodule_eeprom_params params = {};
       struct nl_msg_buff *msgbuff;
       struct nl_socket *nlsk;
       u8 *data = NULL;
       int ret;

       if (netlink_cmd_check(ctx, ETHTOOL_MSG_MODULE_EEPROM_SET, false))
               return -EOPNOTSUPP;
       if (!ctx->argc) {
               fprintf(stderr, "ethtool (--set-module-eeprom): parameters missing\n");
               return 1;
       }

       nlctx->cmd = "--set-module-eeprom";
       nlctx->argp = ctx->argp;
       nlctx->argc = ctx->argc;
       nlctx->devname = ctx->devname;
       nlsk = nlctx->ethnl_socket;
       msgbuff = &nlsk->msgbuff;

       ret = msg_init(nlctx, msgbuff, ETHTOOL_MSG_MODULE_EEPROM_SET,
                      NLM_F_REQUEST | NLM_F_ACK);
       if (ret < 0)
               return 2;
       if (ethnla_fill_header(msgbuff, ETHTOOL_A_MODULE_EEPROM_HEADER,
                              ctx->devname, 0))
               return -EMSGSIZE;

       ret = nl_parser(nlctx, setmodule_params, &params, PARSER_GROUP_NONE, NULL);
       if (ret < 0)
               return 1;

       if (test_bit(PARAM_S_VALUE, &params.present)) {
               params.length = 1;
               data = &params.value;
       } else {
               if (!params.length) {
                       fprintf(stderr, "length missing\n");
                       return 1;
               }
               data = malloc(params.length);
               if (!data)
                       return -ENOMEM;
               if (fread(data, params.length, 1, stdin) != 1) {
                       fprintf(stderr, "not enough data from stdin\n");
                       free(data);
                       return 1;
               }
       }

       if (ethnla_put_u32(msgbuff, ETHTOOL_A_MODULE_EEPROM_LENGTH, params.length) ||
           ethnla_put_u32(msgbuff, ETHTOOL_A_MODULE_EEPROM_OFFSET, params.offset) ||
           ethnla_put_u8(msgbuff, ETHTOOL_A_MODULE_EEPROM_PAGE, params.page) ||
           ethnla_put_u8(msgbuff, ETHTOOL_A_MODULE_EEPROM_BANK, params.bank) ||
           ethnla_put_u8(msgbuff, ETHTOOL_A_MODULE_EEPROM_I2C_ADDRESS,
                         params.i2c_address ?: ETH_I2C_ADDRESS_LOW) ||
           ethnla_put(msgbuff, ETHTOOL_A_MODULE_EEPROM_DATA, params.length, data)) {
               ret = -EMSGSIZE;
               goto out_free;
       }

       ret = nlsock_sendmsg(nlsk, NULL);
       if (ret < 0)
               goto out_free;
       ret = nlsock_process_reply(nlsk, nomsg_reply_cb, nlctx);

out_free:
       if (!test_bit(PARAM_S_VALUE, &params.present))
               free(data);
       if (ret == 0)
               return 0;
       else
               return nlctx->exit_code ?: 83;
}
