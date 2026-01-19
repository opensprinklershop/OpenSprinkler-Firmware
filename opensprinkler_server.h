/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Server functions
 * Feb 2015 @ OpenSprinkler.com
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

#ifndef _OPENSPRINKLER_SERVER_H
#define _OPENSPRINKLER_SERVER_H

#if !defined(ARDUINO)
#include <stdarg.h>
#include <cmath>
	#if defined(OSPI)
	#include <unistd.h>
	#endif
#else
#include <math.h>
#endif

#if defined(ESP8266) || defined(ESP32) || defined(OSPI)
#define OTF_ENABLED
#endif

char dec2hexchar(unsigned char dec);

class BufferFiller {
	char *start; //!< Pointer to start of buffer
	char *ptr; //!< Pointer to cursor position
	size_t len;
	size_t remaining() const {
		if (!start || len == 0) return 0;
		const size_t used = (ptr >= start) ? (size_t)(ptr - start) : 0;
		if (used >= (len - 1)) return 0;
		return (len - 1) - used;
	}

	void terminate() {
		if (!start || len == 0) return;
		const size_t used = (ptr >= start) ? (size_t)(ptr - start) : 0;
		start[(used < len) ? used : (len - 1)] = 0;
	}

	void write_char(char c) {
		if (remaining() == 0) {
			terminate();
			return;
		}
		*ptr++ = c;
		*ptr = 0;
	}

	void advance_by_strnlen(size_t max_len) {
		if (max_len == 0) return;
		size_t adv = 0;
		while (adv < max_len && ptr[adv] != 0) adv++;
		ptr += adv;
	}

public:
	BufferFiller () {}
	BufferFiller (char *buf, size_t buffer_len) {
		start = buf;
		ptr = buf;
		len = buffer_len;
		if (start && len) start[0] = 0;
	}

	char* buffer () const { return start; }
	size_t length () const { return len; }
	unsigned int position () const { return ptr - start; }
	char* cursor() const { return ptr; }
	size_t avail() const { return remaining(); }
	void advance(size_t n) {
		if (n == 0) {
			terminate();
			return;
		}
		const size_t adv = (n < remaining()) ? n : remaining();
		ptr += adv;
		terminate();
	}
	void append(const char* buf, size_t n) {
		if (!buf || n == 0) {
			terminate();
			return;
		}
		const size_t to_copy = (n < remaining()) ? n : remaining();
		if (to_copy) {
			memcpy(ptr, buf, to_copy);
			ptr += to_copy;
		}
		terminate();
	}
	void emit_p(PGM_P fmt, ...) {
		va_list ap;
		va_start(ap, fmt);
		for (;;) {
			char c = pgm_read_byte(fmt++);
			if (c == 0)
				break;
			if (c != '$') {
				write_char(c);
				continue;
			}
			c = pgm_read_byte(fmt++);
			switch (c) {
			case 'D':
				if (remaining()) {
					snprintf((char*)ptr, remaining() + 1, "%d", va_arg(ap, int));
					advance_by_strnlen(remaining());
				} else {
					(void)va_arg(ap, int);
					terminate();
				}
				continue;
			case 'E': //Double
				if (remaining()) {
					snprintf((char*)ptr, remaining() + 1, "%10.6lf", va_arg(ap, double));
					advance_by_strnlen(remaining());
				} else {
					(void)va_arg(ap, double);
					terminate();
				}
				continue;
			case 'L':
				if (remaining()) {
					#if defined(OSPI)
						snprintf((char*)ptr, remaining() + 1, "%" PRIu32, va_arg(ap, long));
					#else
						snprintf((char*)ptr, remaining() + 1, "%lu", va_arg(ap, long));
					#endif
					advance_by_strnlen(remaining());
				} else {
					(void)va_arg(ap, long);
					terminate();
				}
				continue;
			case 'S': {
				const char * st = va_arg(ap, const char*);
				if (remaining()) {
					snprintf((char*)ptr, remaining() + 1, "%s", !st ? "" : st);
					advance_by_strnlen(remaining());
				} else {
					terminate();
				}
				continue; }
			case 'X': {
				char d = va_arg(ap, int);
				write_char(dec2hexchar((d >> 4) & 0x0F));
				write_char(dec2hexchar(d & 0x0F));
			}
				continue;
			case 'F': {
				PGM_P s = va_arg(ap, PGM_P);
				char d;
				while ((d = pgm_read_byte(s++)) != 0) {
					if (remaining() == 0) {
						terminate();
						break;
					}
					*ptr++ = d;
				}
				continue;
			}
			case 'O': {
				uint16_t oid = va_arg(ap, int);
				const size_t want = MAX_SOPTS_SIZE;
				const size_t can = remaining() + 1;
				const size_t read_len = (can < want) ? can : want;
				if (read_len) {
					file_read_block(SOPTS_FILENAME, (char*) ptr, oid * MAX_SOPTS_SIZE, read_len);
					((char*)ptr)[read_len - 1] = 0;
					advance_by_strnlen(read_len);
				} else {
					terminate();
				}
			}
				continue;
			default:
				write_char(c);
				continue;
			}
		}
		terminate();
		va_end(ap);
	}
};

void server_influx_get_main();
void free_tmp_memory();
void restore_tmp_memory();

char* urlDecodeAndUnescape(char *buf);
#endif // _OPENSPRINKLER_SERVER_H
