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
#include <errno.h>
#include <err.h>
#include <sysexits.h>
#include <pthread.h>
#include <paths.h> /* _PATH_VARRUN */
#include <getopt.h>

#include "dev_oss_sequencer.h"
#include "sys_utils.h"

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#undef PACKAGE_STRING
#define PACKAGE_STRING			"virtual_oss_sequencer"

#undef PACKAGE_DESCRIPTION
#define PACKAGE_DESCRIPTION		"Create virtual sequencer device"

#define VIRTUAL_SEQ_DEF_VDEV		"sequencer"

static volatile int app_running = 1;


#define CLO_PREFIX_COUNT_MAX	32
typedef struct command_line_options_s {
	int		daemon;
	const char	*pid;
	int		threads;
	const char	*vdev;
	const char	*prefix[CLO_PREFIX_COUNT_MAX];
	size_t		prefix_count;
} cmd_opts_t, *cmd_opts_p;


static struct option long_options[] = {
	{ "help",	no_argument,		NULL,	'?'	},
	{ "daemon",	no_argument,		NULL,	'd'	},
	{ "pid",	required_argument,	NULL,	'p'	},
	{ "threads",	required_argument,	NULL,	't'	},
	{ "vdev",	required_argument,	NULL,	'V'	},
	{ "prefix",	required_argument,	NULL,	'P'	},
	{ NULL,		0,			NULL,	0	}
};

static const char *long_options_descr[] = {
	"				Show help",
	"				Run as daemon",
	"<pid>				PID file name",
	"<cuse_threads>		CUSE threads count. Default: CPU count x2",
	"<virtual_device_name>		New virtual MIDI device base name. Default: " VIRTUAL_SEQ_DEF_VDEV,
	"<out_device_name_prefix>	Output devices name prefix. Use multiple times if you need more than 1 prefix. Default: midi, umidi",
	NULL
};


static int
cmd_opts_parse(int argc, char **argv, struct option *opts,
    cmd_opts_p cmd_opts) {
	int i, ch, opt_idx;
	char opts_str[1024];

	memset(cmd_opts, 0x00, sizeof(cmd_opts_t));
	cmd_opts->vdev = VIRTUAL_SEQ_DEF_VDEV;

	/* Process command line. */
	/* Generate opts string from long options. */
	for (i = 0, opt_idx = 0;
	    NULL != opts[i].name && (int)(sizeof(opts_str) - 1) > opt_idx;
	    i ++) {
		if (0 == opts[i].val)
			continue;
		opts_str[opt_idx ++] = (char)opts[i].val;
		switch (opts[i].has_arg) {
		case optional_argument:
			opts_str[opt_idx ++] = ':';
			__attribute__((fallthrough)); /* PASSTROUTH. */
		case required_argument:
			opts_str[opt_idx ++] = ':';
			break;
		default:
			break;
		}
	}

	opts_str[opt_idx] = 0;
	opt_idx = -1;
	while ((ch = getopt_long_only(argc, argv, opts_str, opts,
	    &opt_idx)) != -1) {
restart_opts:
		switch (opt_idx) {
		case -1: /* Short option to index. */
			for (opt_idx = 0;
			    NULL != opts[opt_idx].name;
			    opt_idx ++) {
				if (ch == opts[opt_idx].val)
					goto restart_opts;
			}
			/* Unknown option. */
			break;
		case 0: /* help */
			return (EINVAL);
		case 1: /* daemon */
			cmd_opts->daemon = 1;
			break;
		case 2: /* pid */
			cmd_opts->pid = optarg;
			break;
		case 3: /* threads */
			cmd_opts->threads = atoi(optarg);
			break;
		case 4: /* vdev */
			cmd_opts->vdev = optarg;
			break;
		case 5: /* prefix */
			if (CLO_PREFIX_COUNT_MAX >= cmd_opts->prefix_count) {
				fprintf(stderr, "Can not add more prefixies, max count is %i!\n",
				    CLO_PREFIX_COUNT_MAX);
				break;
			}
			cmd_opts->prefix[cmd_opts->prefix_count] = optarg;
			cmd_opts->prefix_count ++;
			break;
		default:
			return (EINVAL);
		}
		opt_idx = -1;
	}

	return (0);
}


static void
signal_handler(int sig) {

	switch (sig) {
	case SIGINT:
	case SIGTERM:
	case SIGKILL:
		app_running = 0;
		break;
	case SIGHUP:
	case SIGUSR1:
	case SIGUSR2:
	default:
		break;
	}
}

static void *
cuse_worker_proc(void *arg __unused) {

	while (0 != app_running) {
		if (0 != cuse_wait_and_process())
			break;
	}

	return (NULL);
}


int
main(int argc, char **argv) {
	int error = 0;
	cmd_opts_t cmd_opts;
	struct cuse_dev *seq_dev;
	pthread_t td;
	/* 1 sec = 1000000000 nanoseconds. */
	struct timespec rqts = { .tv_sec = 0, .tv_nsec = 100000000 };

	/* Command line processing. */
	error = cmd_opts_parse(argc, argv, long_options, &cmd_opts);
	if (0 != error) {
		if (-1 == error)
			return (0); /* Handled action. */
		print_usage(argv[0], PACKAGE_STRING, PACKAGE_DESCRIPTION,
		    long_options, long_options_descr);
		return (error);
	}
	/* Handle cmd line options. */
	if (NULL == cmd_opts.vdev) {
		fprintf(stderr, "vdev is required options!\n");
		print_usage(argv[0], PACKAGE_STRING, PACKAGE_DESCRIPTION,
		    long_options, long_options_descr);
		return (-1);
	}
	if (0 == cmd_opts.threads) {
		cmd_opts.threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
		if (-1 == cmd_opts.threads) {
			cmd_opts.threads = 32;
		} else {
			cmd_opts.threads *= 2;
		}
	}
	if (0 == cmd_opts.prefix_count) { /* Default. */
		cmd_opts.prefix[0] = "midi";
		cmd_opts.prefix[1] = "umidi";
		cmd_opts.prefix_count = 2;
	}

	/* Daemonize. */
	if (0 != cmd_opts.daemon) {
		make_daemon();
		signal(SIGINT, signal_handler);
		signal(SIGTERM, signal_handler);
		signal(SIGHUP, signal_handler);
		signal(SIGUSR1, signal_handler);
		signal(SIGUSR2, signal_handler);
		signal(SIGPIPE, SIG_IGN);
	}
	/* PID file. */
	if (NULL != cmd_opts.pid) {
		write_pid(cmd_opts.pid);
	}

	/* CUSE init. */
	if (0 != cuse_init()) {
		errx(EX_OSERR, "Could not connect to CUSE module - %i: %s",
		    errno, strerror(errno));
	}

	seq_dev = vm_dev_oss_sequencer_create(cmd_opts.vdev,
	    cmd_opts.prefix, cmd_opts.prefix_count);
	if (NULL == seq_dev) {
		errx(EX_SOFTWARE, "Could not create '/dev/%s' - %i: %s",
		    cmd_opts.vdev, errno, strerror(errno));
	}

	/* CUSE post init. */
	for (size_t i = 0; i < (size_t)cmd_opts.threads; i ++) {
		pthread_create(&td, NULL, &cuse_worker_proc, NULL);
	}
	/* Wait for signals. */
	while (0 != app_running) {
		nanosleep(&rqts, NULL); /* Ignore early wakeup and errors. */
	}

	vm_dev_oss_sequencer_destroy(seq_dev);

	return (error);
}
