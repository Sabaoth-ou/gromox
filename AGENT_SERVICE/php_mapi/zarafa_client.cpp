// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020 grammm GmbH
// This file is part of Gromox.
#include "php.h"
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/paths.h>
#include <gromox/zcore_rpc.hpp>
#include "ext.hpp"
#include "zarafa_client.h"
#include "rpc_ext.h"
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <cerrno>
#include <cstdint>

static int zarafa_client_connect()
{
	int sockd, len;
	struct sockaddr_un un;
	
	sockd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sockd < 0) {
		return -1;
	}
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	zstrplus str_server(zend_string_init(ZEND_STRL("zcore_socket"), 0));
	auto sockpath = zend_ini_string(const_cast<char *>("mapi.zcore_socket"), sizeof("mapi.zcore_socket") - 1, 0);
	HX_strlcpy(un.sun_path, sockpath != nullptr ? sockpath : PKGRUNDIR "/zcore.sock", sizeof(un.sun_path));
	len = offsetof(struct sockaddr_un, sun_path) + strlen(un.sun_path);
	if (connect(sockd, (struct sockaddr*)&un, len) < 0) {
		close(sockd);
		return -2;
	}
	return sockd;
}

static zend_bool zarafa_client_read_socket(int sockd, BINARY *pbin)
{
	int read_len;
	uint32_t offset;
	uint8_t resp_buff[5];
	
	pbin->pb = NULL;
	while (1) {
		if (NULL == pbin->pb) {
			read_len = read(sockd, resp_buff, 5);
			if (1 == read_len) {
				pbin->cb = 1;
				pbin->pb = sta_malloc<uint8_t>(1);
				if (NULL == pbin->pb) {
					return 0;
				}
				*(uint8_t*)pbin->pb = resp_buff[0];
				return 1;
			} else if (5 == read_len) {
				pbin->cb = *(uint32_t*)(resp_buff + 1) + 5;
				pbin->pb = sta_malloc<uint8_t>(pbin->cb);
				if (NULL == pbin->pb) {
					return 0;
				}
				memcpy(pbin->pb, resp_buff, 5);
				offset = 5;
				if (offset == pbin->cb) {
					return 1;
				}
				continue;
			} else {
				return 0;
			}
		}
		read_len = read(sockd, pbin->pb + offset, pbin->cb - offset);
		if (read_len <= 0) {
			return 0;
		}
		offset += read_len;
		if (offset == pbin->cb) {
			return 1;
		}
	}
}

static zend_bool zarafa_client_write_socket(int sockd, const BINARY *pbin)
{
	int written_len;
	uint32_t offset;
	
	offset = 0;
	while (1) {
		written_len = write(sockd, pbin->pb + offset, pbin->cb - offset);
		if (written_len <= 0) {
			return 0;
		}
		offset += written_len;
		if (offset == pbin->cb) {
			return 1;
		}
	}
}

zend_bool zarafa_client_do_rpc(
	const RPC_REQUEST *prequest,
	RPC_RESPONSE *presponse)
{
	int sockd;
	BINARY tmp_bin;
	
	if (!rpc_ext_push_request(prequest, &tmp_bin)) {
		return 0;
	}
	sockd = zarafa_client_connect();
	if (sockd < 0) {
		efree(tmp_bin.pb);
		return 0;
	}
	if (!zarafa_client_write_socket(sockd, &tmp_bin)) {
		efree(tmp_bin.pb);
		close(sockd);
		return 0;
	}
	efree(tmp_bin.pb);
	if (!zarafa_client_read_socket(sockd, &tmp_bin)) {
		close(sockd);
		return 0;
	}
	close(sockd);
	if (tmp_bin.cb < 5 || tmp_bin.pb[0] != zcore_response::SUCCESS) {
		if (NULL != tmp_bin.pb) {
			efree(tmp_bin.pb);
		}
		return 0;
	}
	presponse->call_id = prequest->call_id;
	tmp_bin.cb -= 5;
	tmp_bin.pb += 5;
	if (!rpc_ext_pull_response(&tmp_bin, presponse)) {
		efree(tmp_bin.pb - 5);
		return 0;
	}
	efree(tmp_bin.pb - 5);
	return 1;
}

uint32_t zarafa_client_setpropval(GUID hsession,
	uint32_t hobject, uint32_t proptag, const void *pvalue)
{
	TAGGED_PROPVAL propval;
	TPROPVAL_ARRAY propvals;
	
	propvals.count = 1;
	propvals.ppropval = &propval;
	propval.proptag = proptag;
	propval.pvalue = deconst(pvalue);
	return zarafa_client_setpropvals(hsession, hobject, &propvals);
}

uint32_t zarafa_client_getpropval(GUID hsession,
	uint32_t hobject, uint32_t proptag, void **ppvalue)
{
	uint32_t result;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	
	proptags.count = 1;
	proptags.pproptag = &proptag;
	result = zarafa_client_getpropvals(hsession,
				hobject, &proptags, &propvals);
	if (result != ecSuccess)
		return result;
	if (0 == propvals.count) {
		*ppvalue = NULL;
	} else {
		*ppvalue = propvals.ppropval[0].pvalue;
	}
	return ecSuccess;
}
