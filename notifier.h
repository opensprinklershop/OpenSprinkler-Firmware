/* OpenSprinkler Unified Firmware
 * Copyright (C) 2015 by Ray Wang (ray@opensprinkler.com)
 *
 * Notifier data structures and functions header file
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


#ifndef _NOTIFIER_H
#define _NOTIFIER_H

#define NOTIF_QUEUE_MAXSIZE 32

// In-memory circular log of the most recent notification events. The mobile app
// polls this (via the /nl endpoint) so the configured events (IFTTT/MQTT/Email)
// can additionally be shown as push/local notifications on iOS and Android.
#define NOTIF_LOG_MAXSIZE 24

#include "OpenSprinkler.h"
#include "types.h"

/** Notifier Node data structure */
struct NotifNodeStruct {
	uint32_t type;
	uint32_t lval;
	float fval;
	uint8_t bval;
	NotifNodeStruct *next;
	NotifNodeStruct(uint32_t t, uint32_t l=0, float f=0.f, uint8_t b=0) : type(t), lval(l), fval(f), bval(b), next(NULL)
	{ }
};

/** Notifier Queue data structure */
class NotifQueue {
public:
	// Insert a new notification element
	static bool add(uint32_t t, uint32_t l=0, float f=0.f, uint8_t b=0);
	// Clear all elements (i.e. empty the queue)
	static void clear();
	// Run/Process elements. By default process 1 at a time. If n<=0, process all.
	static bool run(int n=1);
protected:
	static NotifNodeStruct* head;
	static NotifNodeStruct* tail;
	static unsigned char nqueue;
};

uint32_t get_notif_enabled();

/** Notification event log (for the mobile app to poll and display as push/local notifications) */
struct NotifLogRecord {
	uint32_t id;      // monotonically increasing id (0 = invalid)
	time_os_t time;   // local time the event was recorded
	uint32_t type;    // NOTIFY_* type
	uint32_t lval;
	float fval;
	uint8_t bval;
};

// Record a notification event into the circular log.
void notif_log_add(uint32_t type, uint32_t lval, float fval, uint8_t bval);
// Number of records currently stored.
uint8_t notif_log_count();
// Highest id assigned so far (0 if none).
uint32_t notif_log_lastid();
// Get the record at logical index (0 = oldest). Returns NULL if out of range.
const NotifLogRecord* notif_log_at(uint8_t idx);
// Render a compact english summary text for an event into out.
void notif_render_text(uint32_t type, uint32_t lval, float fval, uint8_t bval, char* out, size_t outlen);
// Map an event type to a mobile-app priority (0=low, 1=medium, 2=high).
uint8_t notif_priority(uint32_t type);

#endif  // _NOTIFIER_H
