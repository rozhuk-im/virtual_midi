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
/* Required for: SNDCTL_MIDI_INFO. */
#if defined(__OpenBSD__)
	#include <soundcard.h>
#else
	#include <sys/soundcard.h>
#endif

#include <inttypes.h>
#include <stdlib.h> /* malloc, exit */
#include <stdio.h> /* snprintf, fprintf */
#include <string.h> /* bcopy, bzero, memcpy, memmove, memset, strerror... */
#include <errno.h>
#include <pthread.h>
#include <cuse.h>
#include <libgen.h> /* basename */

#include "midi_event.h"
#include "dev_midi.h"


#define VM_MAX_DEV_UNIT		16
#define VM_WRITE_BUF_SZ		4096


typedef struct virt_midi_dev_ctx_s {
	struct cuse_dev *	pdev;
	vmb_settings_p		settings;
	volatile ssize_t	ref_cnt;
	char			descr[32]; /* Device description. */
} vm_dev_t, *vm_dev_p;

typedef struct virt_midi_fd_ctx_s {
	pthread_mutex_t		mtx; /* Multiple threads may use fd in same time. */
	vm_dev_p		dev;
	vmb_synth_p		synth;
	vmb_a_drv_p		adriver;
	int			open_fflags;
	volatile int		tx_busy;
	vm_ep_t		parser;
} vm_fd_t, *vm_fd_p;


static void	vm_dev_free(vm_dev_p dev);


static int
vm_open(struct cuse_dev *pdev, int fflags) {
	vm_dev_p dev = cuse_dev_get_priv0(pdev);
	vm_fd_p fd;

	fd = calloc(1, sizeof(vm_fd_t));
	if (NULL == fd)
		return (CUSE_ERR_NO_MEMORY);
	if (0 != pthread_mutex_init(&fd->mtx, NULL))
		goto err_out_mtx;
	fd->open_fflags = fflags;
	fd->dev = dev;
	fd->synth = vm_backend_synth_new(fd->dev->settings);
	if (NULL == fd->synth) {
err_out:
		pthread_mutex_destroy(&fd->mtx);
err_out_mtx:
		free(fd);
		return (CUSE_ERR_NO_MEMORY);
	}
	fd->adriver = vm_backend_audio_driver_new(fd->dev->settings, fd->synth);
	if (NULL == fd->adriver)
		goto err_out;

	fd->dev->ref_cnt ++;
	cuse_dev_set_per_file_handle(pdev, fd);

	return (0);
}

static int
vm_close(struct cuse_dev *pdev, int fflags __unused) {
	vm_fd_p fd = cuse_dev_get_per_file_handle(pdev);

	if (fd == NULL)
		return (CUSE_ERR_INVALID);

	vm_backend_audio_driver_free(fd->adriver);
	vm_backend_synth_free(fd->synth);
	vm_dev_free(fd->dev);
	pthread_mutex_destroy(&fd->mtx);
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
	vm_evt_p evt;

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
		if (0 != error) {
			retval = error;
			break;
		}

		for (size_t j = 0; j < buf_size; j ++) {
			evt = vm_event_parse(&fd->parser, buf[j]);
			if (NULL == evt)
				continue;
			error = vm_backend_event_handle(fd->synth, evt);
			if (0 != error &&
			    EOPNOTSUPP != error) {
				retval = CUSE_ERR_INVALID;
				break;
			}
		}
		retval += buf_size;
	}

	pthread_mutex_unlock(&fd->mtx);

	return (retval);
}

static int
vm_ioctl(struct cuse_dev *pdev, int fflags __unused,
    unsigned long cmd, void *peer_data) {
	int error = 0;
	size_t len;
	union {
		int ival;
#ifdef SNDCTL_MIDI_INFO
		struct midi_info mi;
#endif
	} data;
	vm_dev_p dev = cuse_dev_get_priv0(pdev);

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
#ifdef SNDCTL_MIDI_INFO
	case SNDCTL_MIDI_INFO: /* _IOWR('Q',12, struct midi_info) */
		if (0 != data.mi.device)
			goto err_out;
		memset(&data, 0x00, len);
		strlcpy(data.mi.name, dev->descr, sizeof(data.mi.name));
		//data.mi.device = 0;
		data.mi.dev_type = 0x01; /* From sequencer.c. */
		break;
#endif
	default:
		/* Log unsupported ioctl(). */
err_out:
		error = CUSE_ERR_INVALID;
		break;
	}

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


static void
vm_dev_free(vm_dev_p dev) {

	if (NULL == dev)
		return;
	dev->ref_cnt --;
	if (0 < dev->ref_cnt)
		return;
	vm_backend_settings_free(dev->settings);
	free(dev);
}

struct cuse_dev *
vm_dev_midi_create(const char *dname, vmb_options_p opts) {
	vm_dev_p dev;

	if (NULL == dname || NULL == opts)
		return (NULL);
	dev = calloc(1, sizeof(vm_dev_t));
	if (NULL == dev)
		return (NULL);
	/* Settings. */
	dev->settings = vm_backend_settings_new(opts);
	if (NULL == dev->settings) {
		errno = ENOMEM;
err_out:
		vm_dev_free(dev);
		return (NULL);
	}
	snprintf(dev->descr, sizeof(dev->descr), "Soft MIDI: %s",
	    basename(opts->device));

	for (int i = 0; i < VM_MAX_DEV_UNIT && NULL == dev->pdev; i ++) {
		dev->pdev = cuse_dev_create(&vm_methods,
		    dev, /* param0 */
		    NULL, /* param1 */
		    0 /* root */,
		    0 /* wheel */,
		    0666 /* mode */,
		    "%s%i.0", dname, i);
	}
	if (NULL == dev->pdev)
		goto err_out;
	dev->ref_cnt ++;

	return (dev->pdev);
}

void
vm_dev_midi_destroy(struct cuse_dev *pdev) {
	vm_dev_p dev = cuse_dev_get_priv0(pdev);

	if (NULL != dev) {
		dev->pdev = NULL;
		dev->ref_cnt --;
		vm_dev_free(dev);
	}
	cuse_dev_destroy(pdev);
}
