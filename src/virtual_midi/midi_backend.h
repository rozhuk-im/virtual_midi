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

#ifndef __MIDI_BACKEND_H__
#define __MIDI_BACKEND_H__

#include <sys/param.h>
#include <sys/types.h>
#include <inttypes.h>
#include <errno.h>

#include "midi_event.h"


typedef struct virt_midi_backend_settings_s *vmb_settings_p;
typedef struct virt_midi_backend_synth_s *vmb_synth_p;
typedef struct virt_midi_backend_audio_driver_s *vmb_a_drv_p;

/* https://www.fluidsynth.org/api/settings_audio.html */
typedef struct virt_midi_backend_options_s {
	const char *	driver;
	const char *	device;
	const char *	soundfont;
} vmb_options_t, *vmb_options_p;


vmb_settings_p
vm_backend_settings_new(vmb_options_p opts);
void
vm_backend_settings_free(vmb_settings_p bs);
int
vm_backend_settings_get_device(vmb_settings_p bs, char *buf, size_t buf_size);

vmb_synth_p
vm_backend_synth_new(vmb_settings_p bs);
void
vm_backend_synth_free(vmb_synth_p bsynth);

vmb_a_drv_p
vm_backend_audio_driver_new(vmb_settings_p bs, vmb_synth_p bsynth);
void
vm_backend_audio_driver_free(vmb_a_drv_p badrv);


/* Return values:
 * EINVAL: invalid args.
 * EIO: backend fail to handle event.
 * EOPNOTSUPP: for types (0xF1+) that not handled. Caller may try to handle it.
 * EDOM: unknown MIDI event type.
 */
int
vm_backend_event_handle(vmb_synth_p bsynth, vm_evt_p evt);


#endif /* __MIDI_BACKEND_H__ */
