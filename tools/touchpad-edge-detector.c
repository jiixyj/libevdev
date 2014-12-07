/*
 * Copyright © 2014 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission.  Red
 * Hat makes no representations about the suitability of this software
 * for any purpose.  It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <libevdev/libevdev.h>
#ifdef __linux__
#include <sys/signalfd.h>
#else
#include <sys/event.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

static int
usage(const char *argv0) {
	printf("Usage: %s /dev/input/event0\n", argv0);
	printf("\n");
	printf("This tool reads the touchpad events from the kernel and calculates\n "
	       "the minimum and maximum for the x and y coordinates, respectively.\n");
	return 1;
}

struct dimensions {
	int top, bottom, left, right;
};

static int
print_current_values(const struct dimensions *d)
{
	static int progress;
	char status = 0;

	switch (progress) {
		case 0: status = '|'; break;
		case 1: status = '/'; break;
		case 2: status = '-'; break;
		case 3: status = '\\'; break;
	}

	progress = (progress + 1) % 4;

	printf("\rTouchpad sends:	x [%d..%d], y [%d..%d] %c",
			d->left, d->right, d->top, d->bottom, status);
	return 0;
}

static int
handle_event(struct dimensions *d, const struct input_event *ev) {
	if (ev->type == EV_SYN) {
		return print_current_values(d);
	} else if (ev->type != EV_ABS)
		return 0;

	switch(ev->code) {
		case ABS_X:
		case ABS_MT_POSITION_X:
			d->left = min(d->left, ev->value);
			d->right = max(d->right, ev->value);
			break;
		case ABS_Y:
		case ABS_MT_POSITION_Y:
			d->top = min(d->top, ev->value);
			d->bottom = max(d->bottom, ev->value);
			break;
	}

	return 0;
}

static int
mainloop(struct libevdev *dev, struct dimensions *dim) {
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);

#ifdef __linux__
	struct pollfd fds[2];

	fds[0].fd = libevdev_get_fd(dev);
	fds[0].events = POLLIN;

	fds[1].fd = signalfd(-1, &mask, SFD_NONBLOCK);
	fds[1].events = POLLIN;

	while (poll(fds, 2, -1)) {
		if (fds[1].revents)
			break;
#else
	int kq = kqueue();
	struct kevent evlist[2];

	EV_SET(&evlist[0], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, 0);
	EV_SET(&evlist[1], libevdev_get_fd(dev), EVFILT_READ, EV_ADD, 0, 0, 0);

	kevent(kq, evlist, 2, NULL, 0, NULL);

	while (kevent(kq, NULL, 0, evlist, 1, NULL) == 1) {
		if (evlist[0].filter == EVFILT_SIGNAL)
			break;
#endif
		struct input_event ev;
		int rc;

		do {
			rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
			if (rc == LIBEVDEV_READ_STATUS_SYNC) {
				fprintf(stderr, "Error: cannot keep up\n");
				return 1;
			} else if (rc != -EAGAIN && rc < 0) {
				fprintf(stderr, "Error: %s\n", strerror(-rc));
				return 1;
			} else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
				handle_event(dim, &ev);
			}
		} while (rc != -EAGAIN);
	}

	return 0;
}

int main (int argc, char **argv) {
	int rc;
	int fd;
	const char *path;
	struct libevdev *dev;
	struct dimensions dim;

	if (argc < 2)
		return usage(argv[0]);

	path = argv[1];
	if (path[0] == '-')
		return usage(argv[0]);

	fd = open(path, O_RDONLY|O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Error opening the device: %s\n", strerror(errno));
		return 1;
	}

	rc = libevdev_new_from_fd(fd, &dev);
	if (rc != 0) {
		fprintf(stderr, "Error fetching the device info: %s\n", strerror(-rc));
		return 1;
	}

	if (libevdev_grab(dev, LIBEVDEV_GRAB) != 0) {
		fprintf(stderr, "Error: cannot grab the device, something else is grabbing it.\n");
		fprintf(stderr, "Use 'fuser -v %s' to find processes with an open fd\n", path);
		return 1;
	}
	libevdev_grab(dev, LIBEVDEV_UNGRAB);

	dim.left = INT_MAX;
	dim.right = INT_MIN;
	dim.top = INT_MAX;
	dim.bottom = INT_MIN;

	printf("Touchpad %s on %s\n", libevdev_get_name(dev), path);
	printf("Move one finger around the touchpad to detect the actual edges\n");
	printf("Kernel says:	x [%d..%d], y [%d..%d]\n",
			libevdev_get_abs_minimum(dev, ABS_X),
			libevdev_get_abs_maximum(dev, ABS_X),
			libevdev_get_abs_minimum(dev, ABS_Y),
			libevdev_get_abs_maximum(dev, ABS_Y));

	setbuf(stdout, NULL);

	rc = mainloop(dev, &dim);

	printf("\n");

	libevdev_free(dev);
	close(fd);

	return rc;
}
