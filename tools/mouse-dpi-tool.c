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
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

struct measurements {
	int distance;
	double frequency;
	uint64_t us;
};

static int
usage(const char *argv0) {
	printf("Usage: %s /dev/input/event0\n", argv0);
	printf("\n");
	printf("This tool reads relative events from the kernel and calculates\n"
	       "the distance covered and maximum frequency of the incoming events.\n"
	       "Some mouse devices provide dynamic frequencies, it is\n"
	       "recommended to measure multiple times to obtain the highest value.\n");
	return 1;
}

static inline uint64_t
tv2us(const struct timeval *tv)
{
	return tv->tv_sec * 1000000 + tv->tv_usec;
}

#define EVENT_SIZ 32

static int double_cmp(void const *x, void const *y) {
	double xx = *(double const *)x, yy = *(double const *)y;
	if (xx < yy)
		return -1;
	if (xx > yy)
		return 1;
	return 0;
}

static inline double get_frequency(double event_times[EVENT_SIZ]) {
	double differences[EVENT_SIZ - 1];
	int d_index = 0;

	for (int i = 0; i < EVENT_SIZ; ++i) {
		int next_index = (i + 1) % EVENT_SIZ;
		if (event_times[i] < event_times[next_index] &&
		    d_index < EVENT_SIZ - 1) {
			differences[d_index++] =
			    event_times[next_index] - event_times[i];
		}
	}
	qsort(differences, d_index, sizeof(differences[0]), double_cmp);

	return 1000000.0 / (differences[EVENT_SIZ / 2]);
}

static int
print_current_values(const struct measurements *m)
{
	static int progress = 0;
	char status = 0;

	switch (progress) {
		case 0: status = '|'; break;
		case 1: status = '/'; break;
		case 2: status = '-'; break;
		case 3: status = '\\'; break;
		default:
			status = '?';
			break;
	}

	progress = (progress + 1) % 4;

	printf("\rCovered distance in device units: %8d at frequency %3.1fHz 	%c",
	       abs(m->distance), m->frequency, status);

	return 0;
}

static int
handle_event(struct measurements *m, const struct input_event *ev)
{
	if (ev->type == EV_SYN) {
		const int idle_reset = 3000000; /* us */
		uint64_t last_us = m->us;

		static double event_times[EVENT_SIZ];
		static size_t event_times_index = 0;
		static bool event_list_valid = false;

		m->us = tv2us(&ev->time);

		/* reset after pause */
		if (last_us + idle_reset < m->us) {
			m->frequency = 0.0;
			m->distance = 0;
			event_times_index = 0;
			event_list_valid = false;
		} else {
			event_times[event_times_index++] = m->us;
			if (event_times_index == EVENT_SIZ) {
				event_list_valid = true;
				event_times_index = 0;
			}
			if (event_list_valid) {
				double freq = get_frequency(event_times);
				m->frequency = max(freq, m->frequency);
			}
			return print_current_values(m);
		}

		return 0;
	} else if (ev->type != EV_REL)
		return 0;

	switch(ev->code) {
		case REL_X:
			m->distance += ev->value;
			break;
	}

	return 0;
}

static int
mainloop(struct libevdev *dev, struct measurements *m) {
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
				handle_event(m, &ev);
			}
		} while (rc != -EAGAIN);
	}

	return 0;
}

static void
print_summary(struct measurements *m)
{
	int res;

	printf("Estimated sampling frequency: %dHz\n", (int)m->frequency);
	printf("To calculate resolution, measure physical distance covered\n"
	       "and look up the matching resolution in the table below\n");

	m->distance = abs(m->distance);

	/* If the mouse has more than 2500dpi, the manufacturer usually
	   shows off on their website anyway */
	for (res = 400; res <= 2500; res += 200) {
		double inch = m->distance/(double)res;
		printf("%8dmm	%8.2fin	%8ddpi\n",
		       (int)(inch * 25.4), inch, res);
	}
	printf("If your resolution is not in the list, calculate it with:\n"
	       "\tresolution=%d/inches, or\n"
	       "\tresolution=%d * 25.4/mm\n", m->distance, m->distance);
}

static inline const char*
bustype(int bustype)
{
	const char *bus;

	switch(bustype) {
		case BUS_PCI: bus = "pci"; break;
		case BUS_ISAPNP: bus = "isapnp"; break;
		case BUS_USB: bus = "usb"; break;
		case BUS_HIL: bus = "hil"; break;
		case BUS_BLUETOOTH: bus = "bluetooth"; break;
		case BUS_VIRTUAL: bus = "virtual"; break;
		default: bus = "unknown bus type"; break;
	}

	return bus;
}

int
main (int argc, char **argv) {
	int rc;
	int fd;
	const char *path;
	struct libevdev *dev;
	struct measurements measurements = {0, 0, 0};

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

	printf("Mouse %s on %s\n", libevdev_get_name(dev), path);
	printf("Move the device 250mm/10in or more along the x-axis.\n");
	printf("Pause 3 seconds before movement to reset, Ctrl+C to exit.\n");
	setbuf(stdout, NULL);

	rc = mainloop(dev, &measurements);

	printf("\n");

	print_summary(&measurements);

	printf("\n");
	printf("Entry for hwdb match (replace XXX with the resolution in DPI):\n"
	       "mouse:%s:v%04xp%04x:name:%s:\n"
	       " MOUSE_DPI=XXX@%d\n",
	       bustype(libevdev_get_id_bustype(dev)),
	       libevdev_get_id_vendor(dev),
	       libevdev_get_id_product(dev),
	       libevdev_get_name(dev),
	       (int)measurements.frequency);

	libevdev_free(dev);
	close(fd);

	return rc;
}
