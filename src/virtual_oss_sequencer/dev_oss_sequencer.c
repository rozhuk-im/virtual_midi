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
#include <sys/ioctl.h>
#include <sys/stat.h>
#if defined(__OpenBSD__)
	#include <soundcard.h>
#else
	#include <sys/soundcard.h>
#endif

#include <inttypes.h>
#include <stdlib.h> /* malloc, exit */
#include <stdio.h> /* snprintf, fprintf */
#include <string.h> /* bcopy, bzero, memcpy, memmove, memset, strerror... */
#include <unistd.h> /* close, write, sysconf */
#include <fcntl.h>
#include <errno.h>
#include <dirent.h> /* opendir, readdir */
#include <pthread.h>
#include <cuse.h>

#include "midi_event.h"
#include "dev_oss_sequencer.h"


#define VM_WRITE_BUF_SZ		4096
#define TMR_TIMERBASE		15 /* Internal use: translate ioctl() to event handler. */


typedef struct virt_midi_device_ctx_s {
	int			fd; /* /dev/midiX.X fd. */
	char			descr[32]; /* Device description. */
	char			dev_name[PATH_MAX]; /* Device file name. */
} vm_dev_t, *vm_dev_p;

typedef struct virt_midi_oss_sequencer_fd_ctx_s {
	pthread_mutex_t		mtx; /* Multiple threads may use fd in same time. */
	int			open_fflags;
	volatile int		tx_busy;
	struct timespec		timer_start; /* Timer start time. */
	struct timespec		timer_stop_diff; /* Timer value on stop. */
	uint64_t		timer_base;
	uint64_t		timer_tempo;
	vm_dev_p		devs;
	size_t			devs_count;
} vm_fd_t, *vm_fd_p;


static int
vm_backend_event_write(vm_fd_p fd, const size_t dev, vm_evt_p evt) {
	int error;
	uint8_t buf[(MIDI_SYSEX_MAX_MSG_SIZE + 8)];
	size_t buf_size;
	ssize_t rc = 0;

	if (NULL == fd ||
	    dev >= fd->devs_count ||
	    NULL == evt)
		return (EINVAL);
	error = vm_event_serialize(evt, buf, sizeof(buf), &buf_size);
	if (0 != error)
		return (error);

	for (size_t i = 0; i < buf_size; i += (size_t)rc) {
		rc = write(fd->devs[dev].fd, &buf[i], (buf_size - i));
		if (-1 == rc)
			return (errno);
	}

	return (0);
}

static uint64_t
vm_time_get(vm_fd_p fd) {
	uint64_t ret;
	struct timespec now;

	if (0 != clock_gettime(CLOCK_MONOTONIC, &now))
		return (0);
	timespecsub(&now, &now, &fd->timer_start);
	/* Convert to nanoseconds: 1 sec = 1000000000 nanoseconds. */
	ret = ((uint64_t)now.tv_sec * 1000000000ul);
	ret += ((uint64_t)now.tv_nsec);
	/* Apply base. */
	ret *= fd->timer_base;
	/* Remove nanoseconds. */
	ret /= 1000000000ul;

	return (ret);
}

static void
vm_timer_wait(vm_fd_p fd, const uint64_t ticks, const int wait_abs) {
	int error;
	uint64_t tmp;
	struct timespec when;

	tmp = ((ticks * 60ull * 1000000000ull) / (fd->timer_tempo * fd->timer_base));
	/* 1 sec = 1000000000 nanoseconds. */
	when.tv_sec = (tmp / 1000000000ul);
	when.tv_nsec = (tmp % 1000000000ul);

	if (0 != wait_abs) {
		if (!timespecisset(&fd->timer_start)) /* Timer was not started! */
			return;
		timespecadd(&when, &fd->timer_start, &when);
	}

	while (0 != (error = clock_nanosleep(CLOCK_MONOTONIC,
	    ((0 != wait_abs) ? TIMER_ABSTIME : 0), &when, &when))) {
		if (EINTR != error)
			break;
	}
}


/* Implement /dev/sequencer protocol. */
static size_t
vm_sequencer_event_handle(vm_fd_p fd, const uint8_t *buf,
    const size_t buf_size) {
	const uint8_t *pbuf = buf;
	size_t ev_size = 0;
	vm_evt_t mevt;
	uint8_t dev, p1;
	uint16_t w14;
	uint32_t param;

	if (NULL == fd || NULL == buf || 0 == buf_size)
		return (buf_size);
	ev_size = ((128 <= buf[0]) ? 8 : 4);
	if (buf_size < ev_size) /* Data truncated. */
		return (buf_size);

	/* Parse event. */
	switch (pbuf[0]) {
	case SEQ_MIDIPUTC: /* 5: Pass through to the midi device. */
		dev = pbuf[2];
		if (fd->devs_count <= (size_t)dev)
			goto err_out;
		p1 = pbuf[1];
		/* Send the event to the next link in the chain. */
		if (1 != write(fd->devs[dev].fd, &p1, 1))
			goto err_out;
		break;
	case EV_TIMING: /* 0x81. */
		/* No dev. */
		switch (pbuf[1]) { /* Timer event. */
		case TMR_WAIT_REL: /* 1: SEQ_DELTA_TIME; ticks. */
			memcpy(&param, &pbuf[4], sizeof(param));
			vm_timer_wait(fd, param, 0);
			break;
		case TMR_WAIT_ABS: /* 2: SEQ_WAIT_TIME; ticks. */
			memcpy(&param, &pbuf[4], sizeof(param));
			vm_timer_wait(fd, param, 1);
			break;
		case TMR_STOP: /* 3: SEQ_STOP_TIMER. */
			if (!timespecisset(&fd->timer_start) ||
			    timespecisset(&fd->timer_stop_diff))
				break;
			clock_gettime(CLOCK_MONOTONIC, &fd->timer_stop_diff);
			timespecsub(&fd->timer_stop_diff, &fd->timer_start,
			    &fd->timer_stop_diff);
			break;
		case TMR_START: /* 4: SEQ_START_TIMER. */
			clock_gettime(CLOCK_MONOTONIC, &fd->timer_start);
			break;
		case TMR_CONTINUE: /* 5: SEQ_CONTINUE_TIMER. */
			if (timespecisset(&fd->timer_stop_diff))
				break;
			clock_gettime(CLOCK_MONOTONIC, &fd->timer_start);
			timespecsub(&fd->timer_start, &fd->timer_stop_diff,
			    &fd->timer_start);
			timespecclear(&fd->timer_stop_diff);
			break;
		case TMR_TEMPO: /* 6: SEQ_SET_TEMPO; value. */
			memcpy(&param, &pbuf[4], sizeof(param));
			if (8 > param) {
				param = 8;
			} else if (360 < param) {
				param = 360;
			}
			fd->timer_tempo = param;
			break;
		case TMR_TIMERBASE: /* 15. */
			memcpy(&param, &pbuf[4], sizeof(param));
			if (1 > param) {
				param = 1;
			} else if (1000 < param) {
				param = 1000;
			}
			fd->timer_base = param;
			break;
		//case TMR_ECHO: /* 8: SEQ_ECHO_BACK; key. */
		//case TMR_CLOCK: /* 9. */
		//case TMR_SPP: /* 10: SEQ_SONGPOS; pos. */
		//case TMR_TIMESIG: /* 11: SEQ_TIME_SIGNATURE; sig. */
		default:
			break;
		}
		break;
	case EV_CHN_COMMON: /* 0x92. */
		dev = pbuf[1];
		if (fd->devs_count <= (size_t)dev)
			goto err_out;
		memset(&mevt, 0x00, sizeof(mevt));
		mevt.type = pbuf[2];
		mevt.chan = pbuf[3];
		mevt.p1 = pbuf[4];

		switch (mevt.type) {
		case MIDI_CTL_CHANGE: /* 0xB0. */
			memcpy(&w14, &pbuf[6], sizeof(w14));
			mevt.p2 = w14;
			break;
		case MIDI_PGM_CHANGE: /* 0xC0. */
		case MIDI_CHN_PRESSURE: /* 0xD0. */
			break;
		case MIDI_PITCH_BEND: /* 0xE0. */
			memcpy(&w14, &pbuf[6], sizeof(w14));
			mevt.p1 = w14;
			break;
		default:
			goto err_out;
		}
		vm_backend_event_write(fd, (size_t)dev, &mevt);
		break;
	case EV_CHN_VOICE: /* 0x93. */
		dev = pbuf[1];
		if (fd->devs_count <= (size_t)dev)
			goto err_out;
		memset(&mevt, 0x00, sizeof(mevt));
		mevt.type = pbuf[2];
		mevt.chan = pbuf[3];
		mevt.p1 = pbuf[4];
		mevt.p2 = pbuf[5];

		switch (mevt.type) {
		case MIDI_NOTEOFF: /* 0x80. */
		case MIDI_NOTEON: /* 0x90. */
		case MIDI_KEY_PRESSURE: /* 0xA0. */
			break;
		default:
			goto err_out;
		}
		vm_backend_event_write(fd, (size_t)dev, &mevt);
		break;
	case EV_SYSEX: /* 0x94. */
		dev = pbuf[1];
		if (fd->devs_count <= (size_t)dev)
			goto err_out;
		/* See seq_sysex(). */
		/* Find len. */
		for (param = 0; param < 6 && 0xff != pbuf[2 + param]; param ++)
			;
		memset(&mevt, 0x00, sizeof(mevt));
		mevt.type = MIDI_SYSEX;
		mevt.p1 = param;
		mevt.ex_data = (void*)&pbuf[2];
		/* Post data. */
		vm_backend_event_write(fd, (size_t)dev, &mevt);
		break;
	case SEQ_FULLSIZE: /* 0xfd: Long events. */
		/* TODO: restore code for SEQ_FULLSIZE. */
		ev_size = buf_size; /* Flush. */
		break;
	//case SEQ_ECHO: /* 8. */
	//case EV_SEQ_LOCAL: /* 0x80. */
	//case SEQ_PRIVATE: /* 0xfe. */
	//case SEQ_EXTENDED: /* 0xff. */
	default:
		/* LOG_EV_FMT("Event type %d not handled %d %d %d",
		    pbuf[0], pbuf[1], pbuf[2], pbuf[3]); */
		break;
	}

err_out:
	return (ev_size);
}


/* Returns 1 if the directory entry should be included in the diff, else 0. */
static int
scandir_filter_cb(const struct dirent *de) {

	if (NULL == de ||
	    0 == de->d_fileno)
		return (0);
	/* Only few types allowed. */
	switch (de->d_type) {
	case DT_CHR:
	case DT_LNK:
		break;
	default: /* Filter out all other. */
		return (0);
	}
	/* Always skip "." and "..". */
	if ('.' == de->d_name[0] &&
	    (0 == de->d_name[1] || ('.' == de->d_name[1] && 0 == de->d_name[2])))
		return (0);
	/* Always skip "midistat". */
	if (0 == strncmp("midistat", de->d_name, sizeof(de->d_name)))
		return (0);

	return (1);
}

static int
vm_open(struct cuse_dev *pdev, int fflags) {
	int rc;
	vm_fd_p fd;
	struct midi_info mi;
	struct dirent **dirp = NULL;
	const char **inc_lst = (const char**)cuse_dev_get_priv0(pdev);
	const size_t inc_lst_cnt = (size_t)cuse_dev_get_priv1(pdev);
	void *tptr;

	fd = calloc(1, sizeof(vm_fd_t));
	if (NULL == fd)
		return (CUSE_ERR_NO_MEMORY);
	if (0 != pthread_mutex_init(&fd->mtx, NULL)) {
		free(fd);
		return (CUSE_ERR_NO_MEMORY);
	}
	fd->open_fflags = fflags;
	fd->timer_base = 100;
	fd->timer_tempo = 60;

	rc = scandir("/dev", &dirp, scandir_filter_cb, alphasort);
	if (-1 == rc)
		goto err_out;
	if (NULL != dirp) {
		fd->devs = calloc((size_t)rc, sizeof(vm_dev_t));
		if (NULL == fd->devs)
			goto err_out;
		for (size_t i = 0; i < (size_t)rc; i ++) {
			for (size_t j = 0; j < inc_lst_cnt; j ++) {
				if (dirp[i]->d_name != strnstr(dirp[i]->d_name,
				    inc_lst[j], dirp[i]->d_namlen))
					continue;
				snprintf(fd->devs[fd->devs_count].dev_name,
				    sizeof(fd->devs[0].dev_name),
				    "/dev/%s", dirp[i]->d_name);
				fd->devs[fd->devs_count].fd = open(
				    fd->devs[fd->devs_count].dev_name, O_RDWR);
				if (-1 == fd->devs[fd->devs_count].fd)
					continue;
				if (0 == ioctl(fd->devs[fd->devs_count].fd,
				    SNDCTL_MIDI_INFO, &mi)) {
					strlcpy(fd->devs[fd->devs_count].descr,
					    mi.name, sizeof(fd->devs[0].descr));
				} else {
					snprintf(fd->devs[fd->devs_count].descr,
					    sizeof(fd->devs[0].descr),
					    "H/W MIDI: %s", dirp[i]->d_name);
				}
				fd->devs_count ++;
				break;
			}
			free(dirp[i]);
		}
		free(dirp);
		tptr = reallocarray(fd->devs, (fd->devs_count + 1), sizeof(vm_dev_t));
		if (NULL != tptr) { /* Fail not critical, ignore it. */
			fd->devs = tptr;
		}
	}

	cuse_dev_set_per_file_handle(pdev, fd);

	return (0);

err_out:
	pthread_mutex_destroy(&fd->mtx);
	free(fd);
	return (CUSE_ERR_NO_MEMORY);
}

static int
vm_close(struct cuse_dev *pdev, int fflags __unused) {
	vm_fd_p fd = cuse_dev_get_per_file_handle(pdev);

	if (fd == NULL)
		return (CUSE_ERR_INVALID);

	pthread_mutex_destroy(&fd->mtx);
	if (NULL != fd->devs) {
		for (size_t i = 0; i < fd->devs_count; i ++) {
			close(fd->devs[i].fd);
		}
		free(fd->devs);
	}
	free(fd);
	cuse_dev_set_per_file_handle(pdev, NULL);

	return (0);
}

static int
vm_read(struct cuse_dev *pdev __unused, int fflags __unused,
    void *peer_ptr __unused, int len __unused) {

	return (CUSE_ERR_INVALID);
}

static int
vm_write(struct cuse_dev *pdev, int fflags __unused, const void *peer_ptr,
    int len) {
	vm_fd_p fd = cuse_dev_get_per_file_handle(pdev);
	int error, retval = 0;
	uint8_t buf[VM_WRITE_BUF_SZ];
	size_t buf_size;

	if (fd == NULL)
		return (CUSE_ERR_INVALID);

	pthread_mutex_lock(&fd->mtx);
	if (fd->tx_busy) {
		pthread_mutex_unlock(&fd->mtx);
		return (CUSE_ERR_BUSY);
	}

	for (size_t i = 0; i < (size_t)len; i += sizeof(buf)) {
		fd->tx_busy = 1;
		pthread_mutex_unlock(&fd->mtx);
		buf_size = MIN(sizeof(buf), (size_t)((size_t)len - i));
		error = cuse_copy_in((((const uint8_t*)peer_ptr) + i),
		    &buf, (int)buf_size);
		pthread_mutex_lock(&fd->mtx);
		fd->tx_busy = 0;
		if (error != 0) {
			retval = error;
			break;
		}

		for (size_t j = 0; j < buf_size;) {
			j += vm_sequencer_event_handle(fd,
			    (buf + j), (buf_size - j));
		}
		retval += buf_size;
	}

	pthread_mutex_unlock(&fd->mtx);

	return (retval);
}

static int
vm_ioctl(struct cuse_dev *pdev, int fflags __unused,
    unsigned long cmd, void *peer_data) {
	vm_fd_p fd = cuse_dev_get_per_file_handle(pdev);
	int error = 0, midiunit;
	size_t len;
	vm_evt_t mevt;
	uint8_t event[8] = { EV_TIMING, 0, 0, 0, 0, 0, 0, 0 };
	union {
		int ival;
		struct midi_info mi;
		struct synth_info synthinfo;
		struct seq_event_rec sevent;
	} data;

	if (fd == NULL)
		return (CUSE_ERR_INVALID);

	len = (size_t)IOCPARM_LEN(cmd);
	if (sizeof(data) < len)
		return (CUSE_ERR_INVALID);

	if (0 != (IOC_IN & cmd)) {
		error = cuse_copy_in(peer_data, &data, (int)len);
		if (error)
			return (error);
	} else { /* No in data. */
		memset(&data, 0x00, len);
	}

	pthread_mutex_lock(&fd->mtx);

	switch (cmd) {
	case FIOASYNC: /* _IOW('f', 125, int): set/clear async i/o. */
	case FIONBIO: /* _IOW('f', 126, int): set/clear non-blocking i/o. */
		/* Not implemented. */
		break;
	case FIONREAD: /* _IOR('f', 127, int): get # bytes to read. */
		data.ival = 0;
		break;
	case FIONWRITE: /* _IOR('f', 119, int): get # bytes (yet) to write. */
		data.ival = VM_WRITE_BUF_SZ;
		break;
	case SNDCTL_TMR_TIMEBASE: /* Set timer base. */
		event[1] = TMR_TIMERBASE;
		memcpy(&event[4], &data, 4);
		vm_sequencer_event_handle(fd, event, sizeof(event));
		break;
	case SNDCTL_TMR_START:
		event[1] = TMR_START;
		vm_sequencer_event_handle(fd, event, sizeof(event));
		break;
	case SNDCTL_TMR_STOP:
		event[1] = TMR_STOP;
		vm_sequencer_event_handle(fd, event, sizeof(event));
		break;
	case SNDCTL_TMR_CONTINUE:
		event[1] = TMR_CONTINUE;
		vm_sequencer_event_handle(fd, event, sizeof(event));
		break;
	case SNDCTL_TMR_TEMPO:
		event[1] = TMR_TEMPO;
		memcpy(&event[4], &data, 4);
		vm_sequencer_event_handle(fd, event, sizeof(event));
		break;
	case SNDCTL_TMR_SOURCE:
	case SNDCTL_TMR_METRONOME:
	case SNDCTL_TMR_SELECT:
		/* Not implemented. */
		break;
	case SNDCTL_SEQ_RESET: /* SNDCTL_SEQ_PANIC == SNDCTL_SEQ_RESET */
	case SNDCTL_SEQ_PANIC:
		memset(&mevt, 0x00, sizeof(mevt));
		mevt.type = MIDI_SYSTEM_RESET;
		for (size_t i = 0; i < fd->devs_count; i ++) {
			vm_backend_event_write(fd, i, &mevt);
		}
		break;
	case SNDCTL_SEQ_SYNC:
		if (0 == (FWRITE & fd->open_fflags))
			break;
		/* Not implemented. */
		break;
	case SNDCTL_SYNTH_INFO:
		midiunit = data.synthinfo.device;
		if (fd->devs_count <= (size_t)midiunit)
			goto err_out;
		memset(&data, 0x00, len);
		/* Lookup from app dsp dev list by num. */
		snprintf(data.synthinfo.name, sizeof(data.synthinfo.name),
		    "%s", fd->devs[midiunit].descr);
		data.synthinfo.device = midiunit;
		data.synthinfo.synth_type = SYNTH_TYPE_MIDI;
		//fluid_settings_getint(fd->settings, "synth.chorus.nr",
		//    &data.synthinfo.nr_voices);
		break;
	case SNDCTL_SEQ_CTRLRATE:
		if (0 != data.ival) /* Can't change. */
			goto err_out;
		data.ival = (int)fd->timer_base;
		break;
	//case SNDCTL_SEQ_GETOUTCOUNT:
	//case SNDCTL_SEQ_GETINCOUNT:
	//case SNDCTL_SEQ_PERCMODE:
	//case SNDCTL_FM_LOAD_INSTR:
	case SNDCTL_SEQ_TESTMIDI:
		/* Not implemented. */
		break;
	case SNDCTL_SEQ_RESETSAMPLES:
		/* Not implemented. */
		break;
	case SNDCTL_SEQ_NRSYNTHS:
	case SNDCTL_SEQ_NRMIDIS:
		data.ival = (int)fd->devs_count;
		break;
	case SNDCTL_MIDI_INFO:
		midiunit = data.mi.device;
		if (fd->devs_count <= (size_t)midiunit)
			goto err_out;
		memset(&data, 0x00, len);
		/* Lookup from app dsp dev list by num. */
		snprintf(data.mi.name, sizeof(data.mi.name),
		    "%s", fd->devs[midiunit].descr);
		data.mi.device = midiunit;
		data.mi.dev_type = 0x01; /* From sequencer.c. */
		break;
	case SNDCTL_SEQ_TRESHOLD:
		/* Not implemented. */
		break;
	//case SNDCTL_SYNTH_MEMAVL:
	case SNDCTL_FM_4OP_ENABLE:
	case SNDCTL_PMGR_ACCESS:
	case SNDCTL_PMGR_IFACE:
		/* Patch manager and fm are ded, ded, ded. */
		goto err_out;
	case SNDCTL_SEQ_OUTOFBAND:
		vm_sequencer_event_handle(fd, (const uint8_t*)&data, len);
		break;
	case SNDCTL_SEQ_GETTIME:
		data.ival = (int)vm_time_get(fd);
		break;
	default:
		/* Log unsupported ioctl(). */
err_out:
		error = CUSE_ERR_INVALID;
		break;
	}
	pthread_mutex_unlock(&fd->mtx);

	if (0 == error &&
	    (0 != (IOC_OUT & cmd))) {
		error = cuse_copy_out(&data, peer_data, (int)len);
	}

	return (error);
}

static int
vm_poll(struct cuse_dev *pdev, int fflags __unused, int events) {
	int retval = CUSE_POLL_NONE;
	vm_fd_p fd = cuse_dev_get_per_file_handle(pdev);

	if (fd == NULL)
		return (retval);

	pthread_mutex_lock(&fd->mtx);
	if (0 != (CUSE_POLL_WRITE & events) &&
	    0 == fd->tx_busy) {
		retval |= CUSE_POLL_WRITE;
	}
	pthread_mutex_unlock(&fd->mtx);

	return (retval);
}


static const struct cuse_methods vm_methods = {
	.cm_open = vm_open,
	.cm_close = vm_close,
	.cm_read = vm_read,
	.cm_write = vm_write,
	.cm_ioctl = vm_ioctl,
	.cm_poll = vm_poll,
};

struct cuse_dev *
vm_dev_oss_sequencer_create(const char *dname, const char **inc_lst,
    const size_t inc_lst_cnt) {
	struct cuse_dev *pdev;

	pdev = cuse_dev_create(&vm_methods,
	    inc_lst, /* param0 */
	    (void*)inc_lst_cnt, /* param1 */
	    0 /* root */,
	    0 /* wheel */,
	    0666 /* mode */,
	    "%s", dname);
	return (pdev);
}

void
vm_dev_oss_sequencer_destroy(struct cuse_dev *pdev) {

	if (NULL == pdev)
		return;
	cuse_dev_destroy(pdev);
}
