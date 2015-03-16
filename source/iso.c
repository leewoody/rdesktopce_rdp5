/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   Protocol services - ISO layer
   Copyright (C) Matthew Chapman <matthewc.unsw.edu.au> 1999-2008
   Copyright 2005-2011 Peter Astrand <astrand@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "rdesktop.h"

extern RD_BOOL g_encryption;
extern RDP_VERSION g_rdp_version;

static RD_BOOL g_negotiate_rdp_protocol = True;

/* Send a self-contained ISO PDU */
static void
iso_send_msg(uint8 code)
{
	STREAM s;

	s = tcp_init(11);

	out_uint8(s, 3);	/* version */
	out_uint8(s, 0);	/* reserved */
	out_uint16_be(s, 11);	/* length */

	out_uint8(s, 6);	/* hdrlen */
	out_uint8(s, code);
	out_uint16(s, 0);	/* dst_ref */
	out_uint16(s, 0);	/* src_ref */
	out_uint8(s, 0);	/* class */

	s_mark_end(s);
	tcp_send(s);
}

static void
iso_send_connection_request(char *username)
{
	STREAM s;
	int length = 30 + strlen(username);

	if (g_rdp_version >= RDP_V5 && g_negotiate_rdp_protocol)
		length += 8;

	s = tcp_init(length);

	out_uint8(s, 3);	/* version */
	out_uint8(s, 0);	/* reserved */
	out_uint16_be(s, length);	/* length */

	out_uint8(s, length - 5);	/* hdrlen */
	out_uint8(s, ISO_PDU_CR);
	out_uint16(s, 0);	/* dst_ref */
	out_uint16(s, 0);	/* src_ref */
	out_uint8(s, 0);	/* class */

	out_uint8p(s, "Cookie: mstshash=", strlen("Cookie: mstshash="));
	out_uint8p(s, username, strlen(username));

	out_uint8(s, 0x0d);	/* cookie termination string: CR+LF */
	out_uint8(s, 0x0a);

	if (g_rdp_version >= RDP_V5 && g_negotiate_rdp_protocol)
	{
		/* optional rdp protocol negotiation request for RDPv5 */
		out_uint8(s, RDP_NEG_REQ);
		out_uint8(s, 0);
		out_uint16(s, 8);
		out_uint32(s, PROTOCOL_SSL);
	}

	s_mark_end(s);
	tcp_send(s);
}

/* Receive a message on the ISO layer, return code */
static STREAM
iso_recv_msg(uint8 * code, uint8 * rdpver)
{
	STREAM s;
	uint16 length;
	uint8 version;

	s = tcp_recv(NULL, 4);
	if (s == NULL)
		return NULL;
	in_uint8(s, version);
	if (rdpver != NULL)
		*rdpver = version;
	if (version == 3)
	{
		in_uint8s(s, 1);	/* pad */
		in_uint16_be(s, length);
	}
	else
	{
		in_uint8(s, length);
		if (length & 0x80)
		{
			length &= ~0x80;
			next_be(s, length);
		}
	}
	if (length < 4)
	{
		error("Bad packet header\n");
		return NULL;
	}
	s = tcp_recv(s, length - 4);
	if (s == NULL)
		return NULL;
	if (version != 3)
		return s;
	in_uint8s(s, 1);	/* hdrlen */
	in_uint8(s, *code);
	if (*code == ISO_PDU_DT)
	{
		in_uint8s(s, 1);	/* eot */
		return s;
	}
	in_uint8s(s, 5);	/* dst_ref, src_ref, class */
	return s;
}

/* Initialise ISO transport data packet */
STREAM
iso_init(int length)
{
	STREAM s;

	s = tcp_init(length + 7);
	s_push_layer(s, iso_hdr, 7);

	return s;
}

/* Send an ISO data PDU */
void
iso_send(STREAM s)
{
	uint16 length;

	s_pop_layer(s, iso_hdr);
	length = s->end - s->p;

	out_uint8(s, 3);	/* version */
	out_uint8(s, 0);	/* reserved */
	out_uint16_be(s, length);

	out_uint8(s, 2);	/* hdrlen */
	out_uint8(s, ISO_PDU_DT);	/* code */
	out_uint8(s, 0x80);	/* eot */

	tcp_send(s);
}

/* Receive ISO transport data packet */
STREAM
iso_recv(uint8 * rdpver)
{
	STREAM s;
	uint8 code = 0;

	s = iso_recv_msg(&code, rdpver);
	if (s == NULL)
		return NULL;
	if (rdpver != NULL)
		if (*rdpver != 3)
			return s;
	if (code != ISO_PDU_DT)
	{
		error("expected DT, got 0x%x\n", code);
		return NULL;
	}
	return s;
}

/* Establish a connection up to the ISO layer */
RD_BOOL
iso_connect(char *server, char *username, RD_BOOL reconnect, uint32 * selected_protocol)
{
	STREAM s;
	uint8 code;

	g_negotiate_rdp_protocol = True;

      retry:
	*selected_protocol = PROTOCOL_RDP;
	code = 0;

	if (!tcp_connect(server))
		return False;

	if (reconnect)
	{
		iso_send_msg(ISO_PDU_CR);
	}
	else
	{
		iso_send_connection_request(username);
	}

	s = iso_recv_msg(&code, NULL);
	if (s == NULL)
		return False;

	if (code != ISO_PDU_CC)
	{
		error("expected CC, got 0x%x\n", code);
		tcp_disconnect();
		return False;
	}

	if (g_rdp_version >= RDP_V5 && s_check_rem(s, 8))
	{
		/* handle RDP_NEG_REQ response */
		const char *reason = NULL;

		uint8 type = 0, flags = 0;
		uint16 length = 0;
		uint32 data = 0;

		in_uint8(s, type);
		in_uint8(s, flags);
		in_uint16(s, length);
		in_uint32(s, data);

		if (type == RDP_NEG_FAILURE)
		{
			switch (data)
			{
				case SSL_REQUIRED_BY_SERVER:
					reason = "SSL required by server";
					break;
				case SSL_WITH_USER_AUTH_REQUIRED_BY_SERVER:
					reason = "SSL with user authentication required by server";
					break;
				case SSL_NOT_ALLOWED_BY_SERVER:
					reason = "SSL not allowed by server";
					break;
				case SSL_CERT_NOT_ON_SERVER:
					reason = "SSL certificated not on server";
					break;
				case INCONSISTENT_FLAGS:
					reason = "inconsistent flags";
					break;
				case HYBRID_REQUIRED_BY_SERVER:
					reason = "hybrid authentication (CredSSP) required by server";
					break;
				default:
					reason = "unknown reason";
			}

			tcp_disconnect();
			warning("RDP protocol negotiation failed with reason: %s (error 0x%x),\n",
				reason, data);
			warning("retrying without negotiation using plain RDP protocol.\n");

			g_negotiate_rdp_protocol = False;
			goto retry;
		}

		if (type != RDP_NEG_RSP)
		{
			tcp_disconnect();
			error("expected RDP_NEG_RSP, got type = 0x%x\n", type);
			warning("retrying without negotiation using plain RDP protocol.\n");

			g_negotiate_rdp_protocol = False;
			goto retry;
		}

		/* handle negotiation response */
		if (data == PROTOCOL_SSL)
		{
			DEBUGMSG(1,(L"iso_connect: negotiation: PROTOCOL_SSL\n"));
			if (!tcp_tls_connect())
			{
				tcp_disconnect();
				DEBUGMSG(1,(L"iso_connect: negotiation: PROTOCOL_SSL FAILED\n"));
				return False;
			}

			/* do not use encryption when using TLS */
			g_encryption = False;
		}
		else if (data != PROTOCOL_RDP)
		{
			tcp_disconnect();
			error("unexpected protocol in neqotiation response, got data = 0x%x.\n",
			      data);
			return False;
		}

		*selected_protocol = data;
	}
	return True;
}

/* Disconnect from the ISO layer */
void
iso_disconnect(void)
{
	iso_send_msg(ISO_PDU_DR);
	tcp_disconnect();
}

/* reset the state to support reconnecting */
void
iso_reset_state(void)
{
	tcp_reset_state();
}
