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
#include <sys/fcntl.h>
#include <sys/stat.h> /* S_IWRITE */

#include <inttypes.h>
#include <stdlib.h> /* malloc, exit */
#include <stdio.h> /* snprintf, fprintf */
#include <string.h> /* bcopy, bzero, memcpy, memmove, memset, strerror... */
#include <unistd.h> /* close, write, sysconf */
#include <errno.h>
#include <err.h>
#include <sysexits.h>
#include <libgen.h> /* basename */
#include <pwd.h>
#include <grp.h>

#include "sys_utils.h"


void
print_usage(char *progname, const char *pkg_name, const char *pkg_descr,
    struct option *opts, const char **opts_descr) {

	fprintf(stderr, "%s     %s\nUsage: %s [options]\noptions:\n",
	    pkg_name, pkg_descr, basename(progname));
	for (size_t i = 0; NULL != opts[i].name; i ++) {
		if (0 == opts[i].val) {
			fprintf(stderr, "	-%s %s\n",
			    opts[i].name, opts_descr[i]);
		} else {
			fprintf(stderr, "	-%s, -%c %s\n",
			    opts[i].name, opts[i].val, opts_descr[i]);
		}
	}
}

void
make_daemon(void) {
	int nullfd;
	char *err_source = NULL;

	switch (fork()) {
	case -1:
		err_source = "fork()";
err_out:
		errx(EX_OSERR, "make_daemon: %s failed - %i: %s",
		    err_source, errno, strerror(errno));
		/* return; */
	case 0: /* Child. */
		break;
	default: /* Parent. */
		exit(0);
	}

	/* Child... */
	setsid();
	setpgid(getpid(), 0);
	chdir("/");

	/* Close stdin, stdout, stderr. */
	nullfd = open("/dev/null", O_RDWR);
	if (-1 == nullfd) {
		err_source = "open(\"/dev/null\")";
		goto err_out;
	}
	dup2(nullfd, STDIN_FILENO);
	dup2(nullfd, STDOUT_FILENO);
	dup2(nullfd, STDERR_FILENO);
	if (STDERR_FILENO < nullfd) {
		close(nullfd);
	}
}

int
write_pid(const char *file_name) {
	int rc, fd;
	char data[16];
	ssize_t ios;

	if (NULL == file_name)
		return (EINVAL);

	rc = snprintf(data, sizeof(data), "%d", getpid());
	if (0 > rc || sizeof(data) <= (size_t)rc)
		return (EFAULT);
	fd = open(file_name, (O_WRONLY | O_CREAT | O_TRUNC), 0644);
	if (-1 == fd)
		return (errno);
	ios = write(fd, data, (size_t)rc);
	if ((size_t)ios != (size_t)rc) {
		close(fd);
		unlink(file_name);
		return (errno);
	}
	fchmod(fd, (S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH));
	close(fd);

	return (0);
}

int
set_user_and_group(uid_t pw_uid, gid_t pw_gid) {
	int error;
	struct passwd *pwd, pwd_buf;
	char buffer[4096], err_descr[256];

	if (0 == pw_uid || 0 == pw_gid)
		return (EINVAL);

	error = getpwuid_r(pw_uid, &pwd_buf, buffer, sizeof(buffer), &pwd);
	if (0 != error) {
		strerror_r(error, err_descr, sizeof(err_descr));
		fprintf(stderr, "set_user_and_group: getpwuid_r() error %i: %s\n",
		    error, err_descr);
		return (error);
	}

	if (0 != setgid(pw_gid)) {
		error = errno;
		strerror_r(error, err_descr, sizeof(err_descr));
		fprintf(stderr, "set_user_and_group: setgid() error %i: %s\n",
		    error, err_descr);
		return (error);
	}
	if (0 != initgroups(pwd->pw_name, pw_gid)) {
		error = errno;
		strerror_r(error, err_descr, sizeof(err_descr));
		fprintf(stderr, "set_user_and_group: initgroups() error %i: %s\n",
		    error, err_descr);
		return (error);
	}
	if (0 != setgroups(1, &pwd->pw_gid)) {
		error = errno;
		strerror_r(error, err_descr, sizeof(err_descr));
		fprintf(stderr, "set_user_and_group: setgroups() error %i: %s\n",
		    error, err_descr);
		return (error);
	}
	if (0 != setuid(pw_uid)) {
		error = errno;
		strerror_r(error, err_descr, sizeof(err_descr));
		fprintf(stderr, "set_user_and_group: setuid() error %i: %s\n",
		    error, err_descr);
		return (error);
	}

	return (0);
}
