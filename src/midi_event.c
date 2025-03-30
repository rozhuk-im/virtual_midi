/*-
 * Copyright (c) 2024-2025 Rozhuk Ivan <rozhuk.im@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Author: Rozhuk Ivan <rozhuk.im@gmail.com>
 *
 */

#include <sys/param.h>
#include <sys/types.h>

#include <inttypes.h>
#include <string.h> /* bcopy, bzero, memcpy, memmove, memset, strerror... */
#include <errno.h>

#include "midi_event.h"


/* Does not include type (status) byte. */
static const size_t midi_type2len_tbl[8] = {
	2,	/* 0x8x: Note off. */
	2,	/* 0x9x: Note on. */
	2,	/* 0xAx: Polyphonic key pressure (After-touch). */
	2,	/* 0xBx: Control change. */
	1,	/* 0xCx: Program change. */
	1,	/* 0xDx: Channel pressure (After-touch). */
	2,	/* 0xEx: Pitch wheel change. */
	0	/* 0xFx: System messages. */
};
/* Drop low 4 bits by shift, and hi bit by "& 0x07", same as "(& 0x0f) - 8". */
#define MIDI_TYPE2LEN(_b)	(midi_type2len_tbl[((_b) >> 4) & 0x07])

/* Does not include type (status) byte. */
static const size_t midi_systype2len_tbl[16] = {
	0,	/* 0xF0: MIDI_SYSEX - special processing. */
	1,	/* 0xF1: MIDI_TIME_CODE. */
	2,	/* 0xF2: MIDI_SONG_POSITION. */
	1,	/* 0xF3: MIDI_SONG_SELECT. */
	0,	/* 0xF4: System Common - undefined. */
	0,	/* 0xF5: System Common - undefined. */
	0,	/* 0xF6: MIDI_TUNE_REQUEST. */
	0,	/* 0xF7: MIDI_SYSEX_EOX. */
	0,	/* 0xF8: MIDI_SYNC. */
	0,	/* 0xF9: Sys real time undefined - MIDI_TICK. */
	0,	/* 0xFA: MIDI_START. */
	0,	/* 0xFB: MIDI_CONTINUE. */
	0,	/* 0xFC: MIDI_STOP. */
	0,	/* 0xFD: Sys real time undefined. */
	0,	/* 0xFE: MIDI_ACTIVE_SENSING. */
	0	/* 0xFF: MIDI_SYSTEM_RESET. */
};
/* Use only low 4 bits. */
#define MIDI_SYSTYPE2LEN(_b)	(midi_systype2len_tbl[(_b) & 0x0F])


int
vm_event_sysex_data_chk(const uint8_t *buf, const size_t buf_size) {

	if (NULL == buf && 0 != buf_size)
		return (EINVAL);
	for (size_t i = 0; i < buf_size; i ++) {
		if (0x7f < buf[i])
			return (EDOM);
	}
	return (0);
}

vm_evt_p
vm_event_parse(vm_ep_p ep, const uint8_t c) {
	vm_evt_p evt = NULL;

	/* Is status byte? */
	if (0x80 & c) {
		/* 0xF8-0xFF: Real-time message. */
		if (0xF8 <= c) {
flush_sys_event:
			ep->type = 0; /* Drop prev incomplete event. */
			memset(&ep->event, 0x00, sizeof(ep->event));
			ep->event.type = c;
			return (&ep->event);
		}
		/* Any status byte terminates SYSEX messages (not just 0xF7). */
		if (MIDI_SYSEX == ep->type &&
		    0 < ep->data_used) { /* 0xF7: MIDI_SYSEX_EOX handled here. */
			/* Fill event to return SYSEX + data. */
			memset(&ep->event, 0x00, sizeof(ep->event));
			ep->event.type = MIDI_SYSEX;
			ep->event.p1 = (uint32_t)ep->data_used;
			ep->event.ex_data = ep->data;
			evt = &ep->event; /* Shedule event return and continue process. */
		}
		/* Restart parser. */
		ep->data_used = 0; /* Mark buffer as empty. */
		if (MIDI_SYSEX > c) { /* 0x80 <= c < 0xF0: Channel messages. */
			ep->type = (0xF0 & c);
			ep->chan = (0x0F & c);
			ep->data_required = MIDI_TYPE2LEN(ep->type);
		} else { /* System messages. */
			ep->chan = 0; /* No channel. */
			switch (c) {
			case MIDI_SYSEX: /* 0xF0. */
				ep->type = MIDI_SYSEX;
				ep->data_required = sizeof(ep->data);
				break;
			case MIDI_SYSEX_EOX: /* 0xF7: never returned as event. */
				ep->type = 0; /* Already handled, ignore event. */
				break;
			default: /* 0xF1-0xF6. */
				ep->data_required = MIDI_SYSTYPE2LEN(c);
				/* Return events without data bytes. */
				if (0 == ep->data_required)
					goto flush_sys_event;
				ep->type = c; /* Shedule wait for event data. */
				break;
			}
		}

		return (evt);
	}

	/* Data/parameter byte.
	 * All bytes here without hi bit set: & 0xF7 masked. */

	/* Discard data bytes for events we don't care about */
	if (0 == ep->type)
		return (NULL);

	/* SYSEX: check free buf space. */
	if (sizeof(ep->data) <= ep->data_used) {
		ep->type = 0; /* Drop event. */
		return (NULL);
	}
	/* Store next byte. */
	ep->data[ep->data_used ++] = c;

	/* Is event complete? */
	if (MIDI_SYSEX == ep->type ||
	    ep->data_used < ep->data_required)
		return (NULL);

	/* Event is complete, return it.
	 * Running status byte MIDI feature is also handled here. */
	ep->data_used = 0; /* Reset data size, in case there are additional running status messages. */
	memset(&ep->event, 0x00, sizeof(ep->event));
	ep->event.type = ep->type;
	ep->event.chan = ep->chan;
	ep->event.p1 = ep->data[0];

	switch (ep->type) {
	case MIDI_NOTEOFF: /* 0x80. */
	case MIDI_NOTEON: /* 0x90. */
	case MIDI_KEY_PRESSURE: /* 0xA0. */
	case MIDI_CTL_CHANGE: /* 0xB0. */
		ep->event.p2 = ep->data[1];
		break;
	case MIDI_PGM_CHANGE: /* 0xC0. */
	case MIDI_CHN_PRESSURE: /* 0xD0. */
	case MIDI_TIME_CODE: /* 0xF1. */
	case MIDI_SONG_SELECT: /* 0xF3. */
		/* 1 byte payload. */
		break;
	case MIDI_PITCH_BEND: /* 0xE0. */
	case MIDI_SONG_POSITION: /* 0xF2. */
		/* 14-bit precision. */
		ep->event.p1 |= (((uint32_t)ep->data[1]) << 7);
		break;
	default: /* Should never happen. */
		return (NULL);
	}

	return (&ep->event);
}


int
vm_event_serialize(vm_evt_p evt, uint8_t *buf, const size_t buf_size,
    size_t *buf_size_ret) {
	size_t buf_size_req;

    	if (NULL == evt ||
	    (NULL == buf && 0 != buf_size) ||
	    NULL == buf_size_ret ||
	    (MIDI_SYSEX == evt->type && (NULL == evt->ex_data || 0 == evt->p1)) ||
	    MIDI_SYSEX_EOX == evt->type)
		return (EINVAL);

	/* Required buff size calculation. */
	if (MIDI_SYSEX > evt->type) { /* 0x80 <= type < 0xF0: Channel messages. */
		buf_size_req = (1 + MIDI_TYPE2LEN(evt->type));
	} else { /* System messages. */
		if (MIDI_SYSEX == evt->type) { /* SYSEX. */
			buf_size_req = (1 + (size_t)evt->p1 + 1);
		} else { /* 0xF1+: other.*/
			buf_size_req = (1 + MIDI_SYSTYPE2LEN(evt->type));
		}
	}

	/* Store back required buf size. */
	(*buf_size_ret) = buf_size_req;
	if (buf_size < buf_size_req) /* Is buf space enough? */
		return (ENOBUFS);

	/* Do serialization... */
	if (MIDI_SYSEX > evt->type) { /* 0x80 <= type < 0xF0: Channel messages. */
		buf[0] = (evt->type | evt->chan);
		buf[1] = (0xF7 & evt->p1);
		switch (evt->type) {
		case MIDI_PGM_CHANGE: /* 0xC0. */
		case MIDI_CHN_PRESSURE: /* 0xD0. */
			/* 1 byte payload. */
			break;
		case MIDI_PITCH_BEND: /* 0xE0. */
			/* 14-bit precision. */
			buf[2] = (0xF7 & (evt->p1 >> 7));
			break;
		default: /* All other types except SYSEX. */
			buf[2] = (0xF7 & evt->p2);
			break;
		}
	} else { /* System messages. */
		buf[0] = evt->type;
		switch (evt->type) {
		case MIDI_SYSEX: /* 0xF0. */
			memcpy(&buf[1], evt->ex_data, (size_t)evt->p1);
			buf[(1 + evt->p1)] = MIDI_SYSEX_EOX;
			break;
		case MIDI_TIME_CODE: /* 0xF1. */
		case MIDI_SONG_SELECT: /* 0xF3. */
			buf[1] = (0xF7 & evt->p1);
			break;
		case MIDI_SONG_POSITION: /* 0xF2. */
			buf[1] = (0xF7 & evt->p1);
			buf[2] = (0xF7 & (evt->p1 >> 7));
			break;
		default: /* 0xF4+: other.*/
			break;
		}
	}

	return (0);
}
