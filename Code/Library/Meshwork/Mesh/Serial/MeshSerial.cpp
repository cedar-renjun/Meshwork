/**
 * This file is part of the Meshwork project.
 *
 * Copyright (C) 2013, Sinisha Djukic
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA  02111-1307  USA
 */
#include "Cosa/Types.h"
#include "Cosa/Power.hh"
#include "Cosa/Wireless.hh"
#include "Cosa/RTC.hh"
#include "Cosa/Power.hh"
#include "Cosa/Watchdog.hh"
#include "Cosa/Trace.hh"
#include "Mesh.h"
#include "Mesh/Network/MeshV1.h"
#include "Mesh/Serial/MeshSerial.h"
#include "Cosa/IOStream/Driver/UART.hh"

#define IFMESHSERIALLOG

#ifndef IFMESHSERIALLOG
#define IFMESHSERIALLOG if(false)
#endif

void MeshSerial::respondWCode(serialmsg_t* msg, uint8_t code) {
	m_serial->putchar(msg->seq);
	m_serial->putchar(1);
	m_serial->putchar(code);
	m_serial->flush(SLEEP_MODE_IDLE);
}

void MeshSerial::respondNOK(serialmsg_t* msg, uint8_t error) {
	m_serial->putchar(msg->seq);
	m_serial->putchar(2);
	m_serial->putchar(MSGCODE_NOK);
	m_serial->putchar(error);
	m_serial->flush(SLEEP_MODE_IDLE);
}

void MeshSerial::respondSendACK(serialmsg_t* msg, uint8_t datalen, uint8_t* data) {
	m_serial->putchar(msg->seq);
	m_serial->putchar(2 + datalen);
	m_serial->putchar(MSGCODE_RFSENDACK);
	m_serial->putchar(datalen);
	for ( int i = 0; i < datalen; i ++ )
		m_serial->putchar(data[i]);
	m_serial->flush(SLEEP_MODE_IDLE);
}

bool MeshSerial::processCfgBasic(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 3 ) {
		data_cfgbasic_t* cfgbasic;
		cfgbasic = (data_cfgbasic_t*) msg->data;
		cfgbasic->nwkcaps = m_serial->getchar();
		cfgbasic->delivery = m_serial->getchar();
		cfgbasic->retry = m_serial->getchar();
		result = true;
		respondWCode(msg, MSGCODE_OK);
		result = true;
	} else {
		respondNOK(msg, ERROR_INSUFFICIENT_DATA);
	}
	return result;
}

bool MeshSerial::processCfgNwk(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 3 ) {
		data_cfgnwk_t* cfgnwk;
		cfgnwk = (data_cfgnwk_t*) msg->data;
		cfgnwk->nwkid = (uint16_t) m_serial->getchar() << 8 | m_serial->getchar();
		cfgnwk->nodeid = m_serial->getchar();
		respondWCode(msg, MSGCODE_OK);
		result = true;
	} else {
		respondNOK(msg, ERROR_INSUFFICIENT_DATA);
	}
	return result;
}

bool MeshSerial::processRFInit(serialmsg_t* msg) {
	m_network->begin() ? respondWCode(msg, MSGCODE_OK) : respondNOK(msg, ERROR_GENERAL);
	return true;
}

bool MeshSerial::processRFDeinit(serialmsg_t* msg) {
	m_network->end() ? respondWCode(msg, MSGCODE_OK) : respondNOK(msg, ERROR_GENERAL);
	return true;
}

int MeshSerial::returnACKPayload(uint8_t src, uint8_t port,
													void* buf, uint8_t len,
														void* bufACK, size_t lenACK) {
	int bytes = 0;
	if ( currentmsg != NULL ) {//must be the case, but need a sanity check
		//first, send RFRECV
		m_serial->putchar(currentmsg->seq);
		m_serial->putchar(4 + len);
		m_serial->putchar(MSGCODE_RFRECV);
		m_serial->putchar(src);
		m_serial->putchar(port);
		m_serial->putchar(len);
		for ( int i = 0; i < len; i ++ )//ok, this can be optimized
			m_serial->putchar(((uint8_t*) buf)[i]);
		m_serial->flush(SLEEP_MODE_IDLE);
		
		//now, wait for RFRECVACK
		uint32_t start = RTC::millis();
		while (true) {
			if ( m_serial->available() < 1 )//minimum response size
				MSLEEP(TIMEOUT_RESPONSE/10);
			if ( RTC::since(start) >= TIMEOUT_RESPONSE )
				break;
		}
		if ( m_serial->available() >= 1 )  {
			uint8_t reslen = m_serial->getchar();
			if ( reslen > 0 && reslen <= lenACK && m_serial->available() >= reslen ) {
				for ( int i = 0; i < reslen; i ++ )
					((char*) bufACK)[i] = m_serial->getchar();
				respondWCode(currentmsg, MSGCODE_OK);
				bytes = reslen;
			} else {
				respondNOK(currentmsg, ERROR_TOO_LONG_DATA);
			}
		} else {
			respondNOK(currentmsg, ERROR_INSUFFICIENT_DATA);
		}
	} else {
		respondNOK(currentmsg, ERROR_ILLEGAL_STATE);
	}
	return bytes;
}

bool MeshSerial::processRFStartRecv(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 4 ) {
		data_rfstartrecv_t* rfstartrecv;
		rfstartrecv = (data_rfstartrecv_t*) msg->data;
		uint32_t timeout = rfstartrecv->timeout =
								(uint32_t) m_serial->getchar() << 24 |
								(uint32_t) m_serial->getchar() << 16 |
								(uint32_t) m_serial->getchar() << 8 |
								(uint32_t) m_serial->getchar();
		uint8_t src, port;
		size_t dataLenMax = MeshV1::PAYLOAD_MAX;
		uint8_t* data[dataLenMax];
		currentmsg = msg;
		int res = m_network->recv(src, port, &data, dataLenMax, timeout, this);
		currentmsg = NULL;
		if ( res == Mesh::Network::OK ) {
			//already covered via returnACKPayload 
		} else if ( res == Mesh::Network::OK_MESSAGE_INTERNAL ) {
			respondWCode(msg, MSGCODE_INTERNAL);
		} else if ( res == Mesh::Network::OK_MESSAGE_IGNORED ) {
			respondWCode(msg, MSGCODE_INTERNAL);
		} else {
			respondNOK(msg, ERROR_RECV);
		}
		result = true;
	} else {
		respondNOK(msg, ERROR_INSUFFICIENT_DATA);
	}
	return result;
}

bool MeshSerial::processRFSend(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 3 ) {//minimal msg len
		data_rfsend_t* rfsend;
		rfsend = (data_rfsend_t*) msg->data;
		uint8_t dst = rfsend->dst = m_serial->getchar();
		uint8_t port = rfsend->port = m_serial->getchar();
		uint8_t datalen = rfsend->datalen = m_serial->getchar();
		if ( m_serial->available() >= datalen ) {
			for ( int i = 0; i < datalen; i ++ )//ok, this can be optimized
				rfsend->data[i] = m_serial->getchar();
			size_t maxACKLen = MeshV1::ACK_PAYLOAD_MAX;
			uint8_t bufACK[maxACKLen];
			int res = m_network->send(dst, port, rfsend->data, datalen, bufACK, maxACKLen);
			if ( res == Mesh::Network::OK ) {
				m_serial->putchar(msg->seq);
				m_serial->putchar(2 + datalen);
				m_serial->putchar(MSGCODE_RFSENDACK);
				m_serial->putchar(datalen);
				for ( int i = 0; i < datalen; i ++ )
					m_serial->putchar(bufACK[i]);
				m_serial->flush(SLEEP_MODE_IDLE);
				result = true;
			} else {
				respondNOK(msg, ERROR_SEND);
			}
		} else {
			respondNOK(msg, ERROR_INSUFFICIENT_DATA);
		}
	} else {
		respondNOK(msg, ERROR_INSUFFICIENT_DATA);
	}
	return result;
}

bool MeshSerial::processRFBroadcast(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 2 ) {//minimal msg len
		data_rfbcast_t* rfbcast;
		rfbcast = (data_rfbcast_t*) msg->data;
		uint8_t port = rfbcast->port = m_serial->getchar();
		uint8_t datalen = rfbcast->datalen = m_serial->getchar();
		if ( m_serial->available() >= datalen ) {
			for ( int i = 0; i < datalen; i ++ )//ok, this can be optimized
				rfbcast->data[i] = m_serial->getchar();
			int res = m_network->broadcast(port, rfbcast->data, datalen);
			if ( res == Mesh::Network::OK ) {
				respondWCode(msg, MSGCODE_OK);
				result = true;
			} else {
				respondNOK(msg, ERROR_BCAST);
			}
		} else {
			respondNOK(msg, ERROR_INSUFFICIENT_DATA);
		}
	} else {
		respondNOK(msg, ERROR_INSUFFICIENT_DATA);
	}
	return result;
}

bool MeshSerial::processOneMessage(serialmsg_t* msg) {
	bool result = false;
	if ( m_serial->available() >= 3 ) {//minimal msg len
		msg->seq = m_serial->getchar();//seq
		int len = msg->len = m_serial->getchar();//len
		if ( len >= 0 ) {
			int msgcode = msg->code = m_serial->getchar();//msgcode
			result = true;
			switch ( msgcode ) {
//				case MSGCODE_OK: break;//handled in respective processXYZ
//				case MSGCODE_NOK: msg->data[0] = m_serial->getchar(); break;//handled in respective processXYZ
//				case MSGCODE_UNKNOWN: break;//handled in respective processXYZ
//				case MSGCODE_INTERNAL: break;//only controller->host
				case MSGCODE_CFGBASIC: result = processCfgBasic(msg); break;
				case MSGCODE_CFGNWK: break;
				case MSGCODE_RFINIT: result = processRFInit(msg); break;
				case MSGCODE_RFDEINIT: result = processRFDeinit(msg); break;
//				case MSGCODE_RCRECV: break;//only controller->host
//				case MSGCODE_RFRECVACK: break;//handled within processRFStartRecv
				case MSGCODE_RFSTARTRECV: result = processRFStartRecv(msg); break;
				case MSGCODE_RFSEND: result = processRFSend(msg); break;
				case MSGCODE_RFSEND: result = processRFSend(msg); break;
//				case MSGCODE_RFSENDACK: break;//only controller->host
				case MSGCODE_RFBCAST: result = processRFBroadcast(msg); break;
				default: respondWCode(msg, MSGCODE_UNKNOWN); result = false;
			}
		}
	}
	return result;
}
