/* OpenSprinkler Unified (AVR/RPI/BBB/LINUX) Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Linux Ethernet functions header file
 * This file is based on Richard Zimmerman's sprinklers_pi program
 * Copyright (c) 2013 Richard Zimmerman
 *
 * This file is part of the OpenSprinkler library
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _ETHERPORT_H_
#define _ETHERPORT_H_

#if defined(ARDUINO)

#else // headers for RPI/BBB

#include <stdio.h>
#include <inttypes.h>
#include <ctype.h>
#include <openssl/ssl.h>
#include <defines.h>

#ifdef __APPLE__
#define MSG_NOSIGNAL SO_NOSIGPIPE
#endif

#define TMPBUF 1024*8

class EthernetServer;

class EthernetClient {
public:
	EthernetClient();
	EthernetClient(int sock);
	~EthernetClient();
	int connect(uint8_t ip[4], uint16_t port);
	bool connected();
	void stop();
	int read(uint8_t *buf, size_t size);
	int readBytes(char *buf, size_t size) { return read((uint8_t*)buf, size); };
	int timedRead();
	size_t readBytesUntil(char terminator, char *buffer, size_t length);
	std::string readStringUntil(char value);
	size_t write(const uint8_t *buf, size_t size);
	operator bool();
	int GetSocket()
	{
		return m_sock;
	}
	void flush();
	bool available();
	void setTimeout(int msec);
private:
	uint8_t *tmpbuf = NULL;
	int tmpbufsize = 0;
	int tmpbufidx = 0;
	int m_sock = 0;
	bool m_connected;
	friend class EthernetServer;
};

class EthernetClientSsl {
public:
	EthernetClientSsl();
	EthernetClientSsl(int sock);
	~EthernetClientSsl();
	int connect(uint8_t ip[4], uint16_t port);
	bool connected();
	void stop();
	int read(uint8_t *buf, size_t size);
	size_t write(const uint8_t *buf, size_t size);
	operator bool();
	int GetSocket()
	{
		return m_sock;
	}
private:
	int m_sock;
	SSL* ssl;
	bool m_connected;
	friend class EthernetServer;
};

class EthernetServer {
public:
	EthernetServer(uint16_t port);
	~EthernetServer();

	bool begin();
	EthernetClient available();
private:
	uint16_t m_port;
	int m_sock;
};
#endif

#endif /* _ETHERPORT_H_ */
