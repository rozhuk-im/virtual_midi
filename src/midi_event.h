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

#ifndef __MIDI_EVENT_H__
#define __MIDI_EVENT_H__

#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>


#ifndef MIDI_SYSEX_MAX_MSG_SIZE
#	define MIDI_SYSEX_MAX_MSG_SIZE	1024
#endif


/* MIDI events types. */
/* https://www.masanao.site/staff/iz/formats/midi-event.html */
/* http://midi.teragonaudio.com/tech/midispec.htm */
/* Channel. */
#define MIDI_NOTEOFF		0x80
#define MIDI_NOTEON		0x90
#define MIDI_KEY_PRESSURE	0xA0
#define MIDI_CTL_CHANGE		0xB0
#define MIDI_PGM_CHANGE		0xC0
#define MIDI_CHN_PRESSURE	0xD0
#define MIDI_PITCH_BEND		0xE0
/* System. */
#define MIDI_SYSEX		0xF0
/* System common. */
#define MIDI_TIME_CODE		0xF1
#define MIDI_SONG_POSITION	0xF2
#define MIDI_SONG_SELECT	0xF3
/* System Common - undefined	0XF4 */
/* System Common - undefined	0XF5 */
#define MIDI_TUNE_REQUEST	0xF6
#define MIDI_SYSEX_EOX		0xF7 /* SYSEX data end marker. */
/* System real-time. */
#define MIDI_SYNC		0xF8
/* Sys real time undefined	0xF9 - MIDI_TICK? */
#define MIDI_START		0xFA
#define MIDI_CONTINUE		0xFB
#define MIDI_STOP		0xFC /* Echo back. */
/* Sys real time undefined	0XFD */
#define MIDI_ACTIVE_SENSING	0xFE
#define MIDI_SYSTEM_RESET	0xFF


typedef union midi_event_u {
	uint8_t		u8[8];
	uint32_t	u32;
	uint64_t	u64;
} midi_event_t, *midi_event_p;


typedef struct virt_midi_event_s {
	uint8_t		type; /* MIDI event type. */
	uint8_t		chan; /* MIDI channel. */
	uint32_t	p1; /* First parameter. */
	uint32_t	p2; /* Second parameter. */
	void *		ex_data; /* SYSEX data. */
} vm_evt_t, *vm_evt_p;


typedef struct virt_midi_event_parser_s {
	uint8_t		type; /* MIDI event type. */
	uint8_t		chan; /* MIDI channel. */
	size_t		data_used; /* Number of event bytes stored in data. */
	size_t		data_required; /* How many bytes does the current event type include? */
	vm_evt_t	event; /* The event, that is returned to the MIDI driver. */
	uint8_t		data[MIDI_SYSEX_MAX_MSG_SIZE]; /* SYSEX data or p1, p2 data. */
} vm_ep_t, *vm_ep_p;


vm_evt_p
vm_event_parse(vm_ep_p ep, const uint8_t c);

int
vm_event_sysex_data_chk(const uint8_t *buf, const size_t buf_size);

int
vm_event_serialize(vm_evt_p evt, uint8_t *buf, const size_t buf_size,
    size_t *buf_size_ret);


#endif /* __MIDI_EVENT_H__ */
