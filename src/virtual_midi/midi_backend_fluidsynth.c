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
#include <stdlib.h> /* malloc, exit */
#include <stdio.h> /* snprintf, fprintf */
#include <unistd.h> /* close, write, sysconf */
#include <string.h> /* bcopy, bzero, memcpy, memmove, memset, strerror... */

#include <fluidsynth.h>

#include "midi_backend.h"


vmb_settings_p
vm_backend_settings_new(vmb_options_p opts) {
	char buf[32];
	fluid_settings_t *s;

	if (NULL == opts)
		return (NULL);
	s = new_fluid_settings();
	if (NULL == s)
		return (NULL);

	if (NULL != opts->driver) {
		fluid_settings_setstr(s, "audio.driver", opts->driver);
		if (NULL != opts->device) {
			snprintf(buf, sizeof(buf), "audio.%s.device", opts->driver);
			fluid_settings_setstr(s, buf, opts->device);
		}
	}
	if (NULL != opts->soundfont) {
		fluid_settings_setstr(s, "synth.default-soundfont", opts->soundfont);
	}
	fluid_settings_setint(s, "audio.realtime-prio", 0);

	return ((vmb_settings_p)s);
}

void
vm_backend_settings_free(vmb_settings_p bs) {

	if (NULL == bs)
		return;
	delete_fluid_settings((fluid_settings_t*)bs);
}


int
vm_backend_settings_get_device(vmb_settings_p bs, char *buf, size_t buf_size) {
	int error = EINVAL;
	char buf_tmp[32], *driver = NULL, *device = NULL;

	if (NULL == bs)
		return (EINVAL);
	if (FLUID_OK != fluid_settings_dupstr((fluid_settings_t*)bs,
	    "audio.driver", &driver) ||
	    0 == driver[0])
		goto err_out;
	snprintf(buf_tmp, sizeof(buf_tmp), "audio.%s.device", driver);
	if (FLUID_OK != fluid_settings_dupstr((fluid_settings_t*)bs,
	    buf_tmp, &device) ||
	    0 == device[0])
		goto err_out;
	strlcpy(buf, device, buf_size);
	error = 0;

err_out:
	fluid_free(device);
	fluid_free(driver);

	return (error);
}


vmb_synth_p
vm_backend_synth_new(vmb_settings_p bs) {
	char *str = NULL;
	fluid_synth_t *synth;

	if (NULL == bs)
		return (NULL);
	synth = new_fluid_synth((fluid_settings_t*)bs);
	if (NULL == synth)
		return (NULL);

	/* Load soundfont. */
	if (FLUID_OK == fluid_settings_dupstr((fluid_settings_t*)bs,
	    "synth.default-soundfont", &str) &&
	    0 != str[0]) {
		fluid_synth_sfload(synth, str, 1);
	}
	fluid_free(str);

	return ((vmb_synth_p)synth);
}

void
vm_backend_synth_free(vmb_synth_p bsynth) {

	if (NULL == bsynth)
		return;
	delete_fluid_synth((fluid_synth_t*)bsynth);
}


vmb_a_drv_p
vm_backend_audio_driver_new(vmb_settings_p bs, vmb_synth_p bsynth) {

	if (NULL == bs ||
	    NULL == bsynth)
		return (NULL);
	return ((vmb_a_drv_p)new_fluid_audio_driver((fluid_settings_t*)bs,
	    (fluid_synth_t*)bsynth));
}

void
vm_backend_audio_driver_free(vmb_a_drv_p badrv) {

	if (NULL == badrv)
		return;
	delete_fluid_audio_driver((fluid_audio_driver_t*)badrv);
}


/* fluid_synth_handle_midi_event(). */
int
vm_backend_event_handle(vmb_synth_p bsynth, vm_evt_p evt) {
	fluid_synth_t *synth = (fluid_synth_t*)bsynth;

	if (NULL == bsynth ||
	    NULL == evt)
		return (EINVAL);

	switch (evt->type) {
	case MIDI_NOTEOFF: /* 0x80. */
		return ((FLUID_OK == fluid_synth_noteoff(synth,
		    evt->chan, (int)evt->p1)) ? 0 : EIO); /* p2: vel? */
	case MIDI_NOTEON: /* 0x90. */
		return ((FLUID_OK == fluid_synth_noteon(synth,
		    evt->chan, (int)evt->p1, (int)evt->p2)) ? 0 : EIO);
	case MIDI_KEY_PRESSURE: /* 0xA0. */
		return ((FLUID_OK == fluid_synth_key_pressure(synth,
		    evt->chan, (int)evt->p1, (int)evt->p2)) ? 0 : EIO);
	case MIDI_CTL_CHANGE: /* 0xB0. */
		return ((FLUID_OK == fluid_synth_cc(synth,
		    evt->chan, (int)evt->p1, (int)evt->p2)) ? 0 : EIO);
	case MIDI_PGM_CHANGE: /* 0xC0. */
		return ((FLUID_OK == fluid_synth_program_change(synth,
		    evt->chan, (int)evt->p1)) ? 0 : EIO);
	case MIDI_CHN_PRESSURE: /* 0xD0. */
		return ((FLUID_OK == fluid_synth_channel_pressure(synth,
		    evt->chan, (int)evt->p1)) ? 0 : EIO);
	case MIDI_PITCH_BEND: /* 0xE0. */
		return ((FLUID_OK == fluid_synth_pitch_bend(synth,
		    evt->chan, (int)evt->p1)) ? 0 : EIO);
	case MIDI_SYSEX: /* 0xF0. */
		return ((FLUID_OK == fluid_synth_sysex(synth,
		    (const char*)evt->ex_data, (int)evt->p1,
		    NULL, NULL, NULL, 0)) ? 0 : EIO);
	case MIDI_SYSTEM_RESET: /* 0xFF. */
		return ((FLUID_OK == fluid_synth_system_reset(synth)) ? 0 : EIO);
	default:
		if (0xF8 <= evt->type) /* Real-time messages (0xF8-0xFF) is not handled. */
			return (EOPNOTSUPP);
		break;
	}

	return (EDOM);
}
