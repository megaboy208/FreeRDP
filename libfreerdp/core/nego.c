/**
 * FreeRDP: A Remote Desktop Protocol Client
 * RDP Protocol Security Negotiation
 *
 * Copyright 2011 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>

#include <freerdp/constants.h>
#include <freerdp/utils/memory.h>

#include "tpkt.h"

#include "nego.h"

#include "transport.h"

static const char* const NEGO_STATE_STRINGS[] =
{
	"NEGO_STATE_INITIAL",
	"NEGO_STATE_NLA",
	"NEGO_STATE_TLS",
	"NEGO_STATE_RDP",
	"NEGO_STATE_FAIL",
	"NEGO_STATE_FINAL"
};

static const char PROTOCOL_SECURITY_STRINGS[3][4] =
{
	"RDP",
	"TLS",
	"NLA"
};

boolean nego_security_connect(rdpNego* nego);

/**
 * Negotiate protocol security and connect.
 * @param nego
 * @return
 */

boolean nego_connect(rdpNego* nego)
{
	if (nego->state == NEGO_STATE_INITIAL)
	{
		if (nego->enabled_protocols[PROTOCOL_NLA] > 0)
			nego->state = NEGO_STATE_NLA;
		else if (nego->enabled_protocols[PROTOCOL_TLS] > 0)
			nego->state = NEGO_STATE_TLS;
		else if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
		{
			DEBUG_NEGO("No security protocol is enabled");
			nego->state = NEGO_STATE_FAIL;
		}

		if (!nego->security_layer_negotiation_enabled)
		{
			DEBUG_NEGO("Security Layer Negotiation is disabled");
			/* attempt only the highest enabled protocol (see nego_attempt_*) */
			nego->enabled_protocols[PROTOCOL_NLA] = 0;
			nego->enabled_protocols[PROTOCOL_TLS] = 0;
			nego->enabled_protocols[PROTOCOL_RDP] = 0;
			if(nego->state == NEGO_STATE_NLA)
			{
				nego->enabled_protocols[PROTOCOL_NLA] = 1;
				nego->selected_protocol = PROTOCOL_NLA;
			}
			else if (nego->state == NEGO_STATE_TLS)
			{
				nego->enabled_protocols[PROTOCOL_TLS] = 1;
				nego->selected_protocol = PROTOCOL_TLS;
			}
			else if (nego->state == NEGO_STATE_RDP)
			{
				nego->enabled_protocols[PROTOCOL_RDP] = 1;
				nego->selected_protocol = PROTOCOL_RDP;
			}
		}

		if(!nego_send_preconnection_pdu(nego))
		{
			DEBUG_NEGO("Failed to send preconnection information");
			nego->state = NEGO_STATE_FINAL;
			return false;
		}
	}

	do
	{
		DEBUG_NEGO("state: %s", NEGO_STATE_STRINGS[nego->state]);

		nego_send(nego);

		if (nego->state == NEGO_STATE_FAIL)
		{
			DEBUG_NEGO("Protocol Security Negotiation Failure");
			nego->state = NEGO_STATE_FINAL;
			return false;
		}
	}
	while (nego->state != NEGO_STATE_FINAL);

	DEBUG_NEGO("Negotiated %s security", PROTOCOL_SECURITY_STRINGS[nego->selected_protocol]);

	/* update settings with negotiated protocol security */
	nego->transport->settings->requested_protocols = nego->requested_protocols;
	nego->transport->settings->selected_protocol = nego->selected_protocol;
	nego->transport->settings->negotiationFlags = nego->flags;

	if(nego->selected_protocol == PROTOCOL_RDP)
	{
		nego->transport->settings->encryption = true;
		nego->transport->settings->encryption_method = ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS;
		nego->transport->settings->encryption_level = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
	}

	/* finally connect security layer (if not already done) */
	if(!nego_security_connect(nego))
	{
		DEBUG_NEGO("Failed to connect with %s security", PROTOCOL_SECURITY_STRINGS[nego->selected_protocol]);
		return false;
	}

	return true;
}

/* connect to selected security layer */
boolean nego_security_connect(rdpNego* nego)
{
	if(!nego->tcp_connected)
	{
		nego->security_connected = false;
	}
	else if (!nego->security_connected)
	{
		if (nego->selected_protocol == PROTOCOL_NLA)
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_NLA");
			nego->security_connected = transport_connect_nla(nego->transport);
		}
		else if (nego->selected_protocol == PROTOCOL_TLS )
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_TLS");
			nego->security_connected = transport_connect_tls(nego->transport);
		}
		else if (nego->selected_protocol == PROTOCOL_RDP)
		{
			DEBUG_NEGO("nego_security_connect with PROTOCOL_RDP");
			nego->security_connected = transport_connect_rdp(nego->transport);
		}
		else
		{
			DEBUG_NEGO("cannot connect security layer because no protocol has been selected yet.");
		}
	}
	return nego->security_connected;
}

/**
 * Connect TCP layer.
 * @param nego
 * @return
 */

boolean nego_tcp_connect(rdpNego* nego)
{
	if (!nego->tcp_connected)
		nego->tcp_connected = transport_connect(nego->transport, nego->hostname, nego->port);
	return nego->tcp_connected;
}

/**
 * Connect TCP layer. For direct approach, connect security layer as well.
 * @param nego
 * @return
 */

boolean nego_transport_connect(rdpNego* nego)
{
	nego_tcp_connect(nego);

	if (nego->tcp_connected && !nego->security_layer_negotiation_enabled)
		return nego_security_connect(nego);

	return nego->tcp_connected;
}

/**
 * Disconnect TCP layer.
 * @param nego
 * @return
 */

int nego_transport_disconnect(rdpNego* nego)
{
	if (nego->tcp_connected)
		transport_disconnect(nego->transport);

	nego->tcp_connected = 0;
	nego->security_connected = 0;
	return 1;
}

/**
 * Send preconnection information if enabled.
 * @param nego
 * @return
 */

boolean nego_send_preconnection_pdu(rdpNego* nego)
{
	STREAM* s;
	uint32 cbSize;
	UNICONV* uniconv;
	uint16 cchPCB_times2 = 0;
	char* wszPCB = NULL;

	if (!nego->send_preconnection_pdu)
		return true;

	DEBUG_NEGO("Sending preconnection PDU");

	if (!nego_tcp_connect(nego))
		return false;

	/* it's easier to always send the version 2 PDU, and it's just 2 bytes overhead */
	cbSize = PRECONNECTION_PDU_V2_MIN_SIZE;

	if (nego->preconnection_blob)
	{
		size_t size;
		uniconv = freerdp_uniconv_new();
		wszPCB = freerdp_uniconv_out(uniconv, nego->preconnection_blob, &size);
		cchPCB_times2 = (uint16) size;
		freerdp_uniconv_free(uniconv);
		cchPCB_times2 += 2; /* zero-termination */
		cbSize += cchPCB_times2;
	}

	s = transport_send_stream_init(nego->transport, cbSize);
	stream_write_uint32(s, cbSize); /* cbSize */
	stream_write_uint32(s, 0); /* Flags */
	stream_write_uint32(s, PRECONNECTION_PDU_V2); /* Version */
	stream_write_uint32(s, nego->preconnection_id); /* Id */
	stream_write_uint16(s, cchPCB_times2 / 2); /* cchPCB */

	if (wszPCB)
	{
		stream_write(s, wszPCB, cchPCB_times2); /* wszPCB */
		xfree(wszPCB);
	}

	if (transport_write(nego->transport, s) < 0)
		return false;

	return true;
}

/**
 * Attempt negotiating NLA + TLS security.
 * @param nego
 */

void nego_attempt_nla(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_NLA | PROTOCOL_TLS;

	DEBUG_NEGO("Attempting NLA security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	DEBUG_NEGO("state: %s", NEGO_STATE_STRINGS[nego->state]);
	if (nego->state != NEGO_STATE_FINAL)
	{
		nego_transport_disconnect(nego);

		if (nego->enabled_protocols[PROTOCOL_TLS] > 0)
			nego->state = NEGO_STATE_TLS;
		else if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
			nego->state = NEGO_STATE_FAIL;
	}
}

/**
 * Attempt negotiating TLS security.
 * @param nego
 */

void nego_attempt_tls(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_TLS;

	DEBUG_NEGO("Attempting TLS security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (nego->state != NEGO_STATE_FINAL)
	{
		nego_transport_disconnect(nego);

		if (nego->enabled_protocols[PROTOCOL_RDP] > 0)
			nego->state = NEGO_STATE_RDP;
		else
			nego->state = NEGO_STATE_FAIL;
	}
}

/**
 * Attempt negotiating standard RDP security.
 * @param nego
 */

void nego_attempt_rdp(rdpNego* nego)
{
	nego->requested_protocols = PROTOCOL_RDP;

	DEBUG_NEGO("Attempting RDP security");

	if (!nego_transport_connect(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_send_negotiation_request(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}

	if (!nego_recv_response(nego))
	{
		nego->state = NEGO_STATE_FAIL;
		return;
	}
}

/**
 * Wait to receive a negotiation response
 * @param nego
 */

boolean nego_recv_response(rdpNego* nego)
{
	STREAM* s = transport_recv_stream_init(nego->transport, 1024);

	if (transport_read(nego->transport, s) < 0)
		return false;

	return nego_recv(nego->transport, s, nego);
}

/**
 * Receive protocol security negotiation message.\n
 * @msdn{cc240501}
 * @param transport transport
 * @param s stream
 * @param extra nego pointer
 */

boolean nego_recv(rdpTransport* transport, STREAM* s, void* extra)
{
	uint8 li;
	uint8 type;
	rdpNego* nego = (rdpNego*) extra;

	if (tpkt_read_header(s) == 0)
		return false;

	li = tpdu_read_connection_confirm(s);

	if (li > 6)
	{
		/* rdpNegData (optional) */

		stream_read_uint8(s, type); /* Type */

		switch (type)
		{
			case TYPE_RDP_NEG_RSP:
				nego_process_negotiation_response(nego, s);

				DEBUG_NEGO("selected_protocol: %d", nego->selected_protocol);

				/* enhanced security selected ? */

				if (nego->selected_protocol)
				{
					if ((nego->selected_protocol == PROTOCOL_NLA) &&
							(!nego->enabled_protocols[PROTOCOL_NLA]))
					{
						nego->state = NEGO_STATE_FAIL;
					}
					if ((nego->selected_protocol == PROTOCOL_TLS) &&
						(!nego->enabled_protocols[PROTOCOL_TLS]))
					{
						nego->state = NEGO_STATE_FAIL;
					}
				}
				else if (!nego->enabled_protocols[PROTOCOL_RDP])
				{
					nego->state = NEGO_STATE_FAIL;
				}
				break;

			case TYPE_RDP_NEG_FAILURE:
				nego_process_negotiation_failure(nego, s);
				break;
		}
	}
	else
	{
		DEBUG_NEGO("no rdpNegData");

		if (!nego->enabled_protocols[PROTOCOL_RDP])
			nego->state = NEGO_STATE_FAIL;
		else
			nego->state = NEGO_STATE_FINAL;
	}

	return true;
}

/**
 * Read protocol security negotiation request message.\n
 * @param nego
 * @param s stream
 */

boolean nego_read_request(rdpNego* nego, STREAM* s)
{
	uint8 li;
	uint8 c;
	uint8 type;

	tpkt_read_header(s);
	li = tpdu_read_connection_request(s);

	if (li != stream_get_left(s) + 6)
	{
		printf("Incorrect TPDU length indicator.\n");
		return false;
	}

	if (stream_get_left(s) > 8)
	{
		/* Optional routingToken or cookie, ending with CR+LF */
		while (stream_get_left(s) > 0)
		{
			stream_read_uint8(s, c);
			if (c != '\x0D')
				continue;
			stream_peek_uint8(s, c);
			if (c != '\x0A')
				continue;

			stream_seek_uint8(s);
			break;
		}
	}

	if (stream_get_left(s) >= 8)
	{
		/* rdpNegData (optional) */

		stream_read_uint8(s, type); /* Type */

		if (type != TYPE_RDP_NEG_REQ)
		{
			printf("Incorrect negotiation request type %d\n", type);
			return false;
		}

		nego_process_negotiation_request(nego, s);
	}

	return true;
}

/**
 * Send protocol security negotiation message.
 * @param nego
 */

void nego_send(rdpNego* nego)
{
	if (nego->state == NEGO_STATE_NLA)
		nego_attempt_nla(nego);
	else if (nego->state == NEGO_STATE_TLS)
		nego_attempt_tls(nego);
	else if (nego->state == NEGO_STATE_RDP)
		nego_attempt_rdp(nego);
	else
		DEBUG_NEGO("invalid negotiation state for sending");
}

/**
 * Send RDP Negotiation Request (RDP_NEG_REQ).\n
 * @msdn{cc240500}\n
 * @msdn{cc240470}
 * @param nego
 */

boolean nego_send_negotiation_request(rdpNego* nego)
{
	STREAM* s;
	int length;
	uint8 *bm, *em;

	s = transport_send_stream_init(nego->transport, 256);
	length = TPDU_CONNECTION_REQUEST_LENGTH;
	stream_get_mark(s, bm);
	stream_seek(s, length);

	if (nego->routing_token != NULL)
	{
		stream_write(s, nego->routing_token->data, nego->routing_token->length);
		length += nego->routing_token->length;
	}
	else if (nego->cookie != NULL)
	{
		int cookie_length = strlen(nego->cookie);
		stream_write(s, "Cookie: mstshash=", 17);
		stream_write(s, (uint8*) nego->cookie, cookie_length);
		stream_write_uint8(s, 0x0D); /* CR */
		stream_write_uint8(s, 0x0A); /* LF */
		length += cookie_length + 19;
	}

	DEBUG_NEGO("requested_protocols: %d", nego->requested_protocols);
	if (nego->requested_protocols > PROTOCOL_RDP)
	{
		/* RDP_NEG_DATA must be present for TLS and NLA */
		stream_write_uint8(s, TYPE_RDP_NEG_REQ);
		stream_write_uint8(s, 0); /* flags, must be set to zero */
		stream_write_uint16(s, 8); /* RDP_NEG_DATA length (8) */
		stream_write_uint32(s, nego->requested_protocols); /* requestedProtocols */
		length += 8;
	}

	stream_get_mark(s, em);
	stream_set_mark(s, bm);
	tpkt_write_header(s, length);
	tpdu_write_connection_request(s, length - 5);
	stream_set_mark(s, em);

	if (transport_write(nego->transport, s) < 0)
		return false;

	return true;
}

/**
 * Process Negotiation Request from Connection Request message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_request(rdpNego* nego, STREAM* s)
{
	uint8 flags;
	uint16 length;

	DEBUG_NEGO("RDP_NEG_REQ");

	stream_read_uint8(s, flags);
	stream_read_uint16(s, length);
	stream_read_uint32(s, nego->requested_protocols);

	DEBUG_NEGO("requested_protocols: %d", nego->requested_protocols);

	nego->state = NEGO_STATE_FINAL;
}

/**
 * Process Negotiation Response from Connection Confirm message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_response(rdpNego* nego, STREAM* s)
{
	uint16 length;

	DEBUG_NEGO("RDP_NEG_RSP");

	stream_read_uint8(s, nego->flags);
	stream_read_uint16(s, length);
	stream_read_uint32(s, nego->selected_protocol);

	nego->state = NEGO_STATE_FINAL;
}

/**
 * Process Negotiation Failure from Connection Confirm message.
 * @param nego
 * @param s
 */

void nego_process_negotiation_failure(rdpNego* nego, STREAM* s)
{
	uint8 flags;
	uint16 length;
	uint32 failureCode;

	DEBUG_NEGO("RDP_NEG_FAILURE");

	stream_read_uint8(s, flags);
	stream_read_uint16(s, length);
	stream_read_uint32(s, failureCode);

	switch (failureCode)
	{
		case SSL_REQUIRED_BY_SERVER:
			DEBUG_NEGO("Error: SSL_REQUIRED_BY_SERVER");
			break;
		case SSL_NOT_ALLOWED_BY_SERVER:
			DEBUG_NEGO("Error: SSL_NOT_ALLOWED_BY_SERVER");
			break;
		case SSL_CERT_NOT_ON_SERVER:
			DEBUG_NEGO("Error: SSL_CERT_NOT_ON_SERVER");
			break;
		case INCONSISTENT_FLAGS:
			DEBUG_NEGO("Error: INCONSISTENT_FLAGS");
			break;
		case HYBRID_REQUIRED_BY_SERVER:
			DEBUG_NEGO("Error: HYBRID_REQUIRED_BY_SERVER");
			break;
		default:
			DEBUG_NEGO("Error: Unknown protocol security error %d", failureCode);
			break;
	}

	nego->state = NEGO_STATE_FAIL;
}

/**
 * Send RDP Negotiation Response (RDP_NEG_RSP).\n
 * @param nego
 */

boolean nego_send_negotiation_response(rdpNego* nego)
{
	STREAM* s;
	uint8* bm;
	uint8* em;
	int length;
	boolean status;
	rdpSettings* settings;

	status = true;
	settings = nego->transport->settings;

	s = transport_send_stream_init(nego->transport, 256);
	length = TPDU_CONNECTION_CONFIRM_LENGTH;
	stream_get_mark(s, bm);
	stream_seek(s, length);

	if (nego->selected_protocol > PROTOCOL_RDP)
	{
		/* RDP_NEG_DATA must be present for TLS and NLA */
		stream_write_uint8(s, TYPE_RDP_NEG_RSP);
		stream_write_uint8(s, EXTENDED_CLIENT_DATA_SUPPORTED); /* flags */
		stream_write_uint16(s, 8); /* RDP_NEG_DATA length (8) */
		stream_write_uint32(s, nego->selected_protocol); /* selectedProtocol */
		length += 8;
	}
	else if (!settings->rdp_security)
	{
		stream_write_uint8(s, TYPE_RDP_NEG_FAILURE);
		stream_write_uint8(s, 0); /* flags */
		stream_write_uint16(s, 8); /* RDP_NEG_DATA length (8) */
		/*
		 * TODO: Check for other possibilities,
		 *       like SSL_NOT_ALLOWED_BY_SERVER.
		 */
		printf("nego_send_negotiation_response: client supports only Standard RDP Security\n");
		stream_write_uint32(s, SSL_REQUIRED_BY_SERVER);
		length += 8;
		status = false;
	}

	stream_get_mark(s, em);
	stream_set_mark(s, bm);
	tpkt_write_header(s, length);
	tpdu_write_connection_confirm(s, length - 5);
	stream_set_mark(s, em);

	if (transport_write(nego->transport, s) < 0)
		return false;

	if (status)
	{
		/* update settings with negotiated protocol security */
		settings->requested_protocols = nego->requested_protocols;
		settings->selected_protocol = nego->selected_protocol;

		if (settings->selected_protocol == PROTOCOL_RDP)
		{
			settings->tls_security = false;
			settings->nla_security = false;
			settings->rdp_security = true;

			if (!settings->local)
			{
				settings->encryption = true;
				settings->encryption_method = ENCRYPTION_METHOD_40BIT | ENCRYPTION_METHOD_128BIT | ENCRYPTION_METHOD_FIPS;
				settings->encryption_level = ENCRYPTION_LEVEL_CLIENT_COMPATIBLE;
			}

			if (settings->encryption && settings->server_key == NULL && settings->rdp_key_file == NULL)
				return false;
		}
		else if (settings->selected_protocol == PROTOCOL_TLS)
		{
			settings->tls_security = true;
			settings->nla_security = false;
			settings->rdp_security = false;
			settings->encryption = false;
			settings->encryption_method = ENCRYPTION_METHOD_NONE;
			settings->encryption_level = ENCRYPTION_LEVEL_NONE;
		}
		else if (settings->selected_protocol == PROTOCOL_NLA)
		{
			settings->tls_security = true;
			settings->nla_security = true;
			settings->rdp_security = false;
			settings->encryption = false;
			settings->encryption_method = ENCRYPTION_METHOD_NONE;
			settings->encryption_level = ENCRYPTION_LEVEL_NONE;
		}
	}

	return status;
}

/**
 * Initialize NEGO state machine.
 * @param nego
 */

void nego_init(rdpNego* nego)
{
	nego->state = NEGO_STATE_INITIAL;
	nego->requested_protocols = PROTOCOL_RDP;
	nego->transport->recv_callback = nego_recv;
	nego->transport->recv_extra = (void*) nego;
	nego->flags = 0;
}

/**
 * Create a new NEGO state machine instance.
 * @param transport
 * @return
 */

rdpNego* nego_new(struct rdp_transport * transport)
{
	rdpNego* nego = (rdpNego*) xzalloc(sizeof(rdpNego));

	if (nego != NULL)
	{
		nego->transport = transport;
		nego_init(nego);
	}

	return nego;
}

/**
 * Free NEGO state machine.
 * @param nego
 */

void nego_free(rdpNego* nego)
{
	xfree(nego);
}

/**
 * Set target hostname and port.
 * @param nego
 * @param hostname
 * @param port
 */

void nego_set_target(rdpNego* nego, char* hostname, int port)
{
	nego->hostname = hostname;
	nego->port = port;
}

/**
 * Enable security layer negotiation.
 * @param nego pointer to the negotiation structure
 * @param enable_rdp whether to enable security layer negotiation (true for enabled, false for disabled)
 */

void nego_set_negotiation_enabled(rdpNego* nego, boolean security_layer_negotiation_enabled)
{
	DEBUG_NEGO("Enabling security layer negotiation: %s", security_layer_negotiation_enabled ? "true" : "false");
	nego->security_layer_negotiation_enabled = security_layer_negotiation_enabled;
}

/**
 * Enable RDP security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_rdp whether to enable normal RDP protocol (true for enabled, false for disabled)
 */

void nego_enable_rdp(rdpNego* nego, boolean enable_rdp)
{
	DEBUG_NEGO("Enabling RDP security: %s", enable_rdp ? "true" : "false");
	nego->enabled_protocols[PROTOCOL_RDP] = enable_rdp;
}

/**
 * Enable TLS security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_tls whether to enable TLS + RDP protocol (true for enabled, false for disabled)
 */
void nego_enable_tls(rdpNego* nego, boolean enable_tls)
{
	DEBUG_NEGO("Enabling TLS security: %s", enable_tls ? "true" : "false");
	nego->enabled_protocols[PROTOCOL_TLS] = enable_tls;
}


/**
 * Enable NLA security protocol.
 * @param nego pointer to the negotiation structure
 * @param enable_nla whether to enable network level authentication protocol (true for enabled, false for disabled)
 */

void nego_enable_nla(rdpNego* nego, boolean enable_nla)
{
	DEBUG_NEGO("Enabling NLA security: %s", enable_nla ? "true" : "false");
	nego->enabled_protocols[PROTOCOL_NLA] = enable_nla;
}

/**
 * Set routing token.
 * @param nego
 * @param routing_token
 */

void nego_set_routing_token(rdpNego* nego, rdpBlob* routing_token)
{
	nego->routing_token = routing_token;
}

/**
 * Set cookie.
 * @param nego
 * @param cookie
 */

void nego_set_cookie(rdpNego* nego, char* cookie)
{
	nego->cookie = cookie;
}

/**
 * Enable / disable preconnection PDU.
 * @param nego
 * @param send_pcpdu
 */

void nego_set_send_preconnection_pdu(rdpNego* nego, boolean send_pcpdu)
{
	nego->send_preconnection_pdu = send_pcpdu;
}

/**
 * Set preconnection id.
 * @param nego
 * @param id
 */

void nego_set_preconnection_id(rdpNego* nego, uint32 id)
{
	nego->preconnection_id = id;
}

/**
 * Set preconnection blob.
 * @param nego
 * @param blob
 */

void nego_set_preconnection_blob(rdpNego* nego, char* blob)
{
	nego->preconnection_blob = blob;
}
