/*
 * rwtouch: replay touches through /dev/redwood-touch (see redwood_input.h).
 *
 * usage: rwtouch STEP...
 *   wait SECONDS                 pause
 *   tap X Y                      a short touch
 *   swipe X0 Y0 X1 Y1 [MS]       drag from one point to the other in MS milliseconds (default 300)
 *   drag X0 Y0 X1 Y1 MS HOLD [GRAB]
 *                                the same, then stay at the end HOLD milliseconds before letting go
 *                                (what is dragged is held where it was taken, neither flung nor sprung back);
 *                                GRAB milliseconds of staying still first, for what has to be picked up
 *                                before it will move -- an icon of the home screen wants about 700
 *   hold X Y MS                  touch and stay
 *   zigzag X Y0 Y1 MS N [STEP]   touch at Y0, go to Y1 in MS milliseconds and back again, N times, then let go;
 *                                a sample every STEP milliseconds (default 8, what the touch screen gives, as
 *                                swipe and drag give them):
 *                                for measuring how far behind the finger the screen is (tools/latency.py)
 *   button NAME [MS]             press a button: home, lock, volup, voldown
 *   read                         print the samples that arrive, forever (to watch the queue)
 *   notify NAME                  post a Darwin notification (com.redwood.unlock: see inject_sb.c)
 *   clock                        how long short sleeps really take (a check of the kernel's timers)
 * Positions are fractions of the screen: 0 0 is the top left corner, 1 1 the bottom right.
 * Example, "swipe up from the bottom edge two minutes after boot":
 *   rwtouch wait 120 swipe 0.5 0.99 0.5 0.4 250
 */
#include <fcntl.h>
#include <mach/mach_time.h>
#include <notify.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "redwood_input.h"

static int device;
static mach_timebase_info_data_t timebase;

/* Microseconds on the CPU's own counter. */
static uint64_t
now_us(void)
{
	return mach_absolute_time() * timebase.numer / timebase.denom / 1000;
}

/*
 * A wait on that counter, not with one usleep() of the whole length.
 *
 * On this port a short sleep comes back and a long one does not: 20 sleeps of 1 ms took 25102 us in
 * all (the longest 1279 us), while the first sleep of 16 ms never returned (logs/lancelot-glfps7.log),
 * and the same 16 ms is what the gestures of a run were waiting on in every run before (the next line
 * of etc/rc.jobs never started). So the wait is cut into pieces of a millisecond, which do come back,
 * and the last piece is spun on. mach_absolute_time() is the CPU's own counter and cannot be late,
 * so the wait ends when it should even if a sleep overstays.
 */
static void
wait_us(uint64_t us)
{
	uint64_t until = now_us() + us;

	while (now_us() < until) {
		uint64_t left = until - now_us();

		if (left > 1000) {
			usleep(1000);
		} else {
			while (now_us() < until) {
			}
		}
	}
}

/* A line to the console at once, without stdio's buffer: in a run the log is the only window there is. */
static void
trace(const char *format, ...)
{
	char line[192];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	if (length > 0) {
		size_t size = (size_t)length < sizeof(line) ? (size_t)length : sizeof(line) - 1;

		(void)write(1, line, size);
	}
}

static void
post(uint32_t type, double x, double y)
{
	struct redwood_touch t = { .type = type, .finger = 0, .x = (uint32_t)(x * 65535.0 + 0.5), .y = (uint32_t)(y * 65535.0 + 0.5) };

	if (write(device, &t, sizeof(t)) != (ssize_t)sizeof(t)) {
		perror("rwtouch: write");
		exit(1);
	}
}

static void
button(uint32_t which, int ms)
{
	struct redwood_touch t = { .type = REDWOOD_BUTTON_DOWN, .finger = which };

	if (write(device, &t, sizeof(t)) != (ssize_t)sizeof(t)) {
		perror("rwtouch: write");
		exit(1);
	}
	wait_us((uint64_t)ms * 1000);
	t.type = REDWOOD_BUTTON_UP;
	write(device, &t, sizeof(t));
}

#define SAMPLE_MS       8               /* a sample every so many milliseconds: what the touch screen gives */

/* Until `at` microseconds after `start` (mach_absolute_time()): on a clock of its own, not one sleep after another */
static void
sleep_until(uint64_t start, uint64_t at)
{
	for (;;) {
		uint64_t now = (mach_absolute_time() - start) * timebase.numer / timebase.denom / 1000;

		if (now >= at) {
			return;
		}
		if (at - now > 1000) {
			usleep(1000);           /* see wait_us(): a millisecond comes back, 16 of them do not */
		}
	}
}

/*
 * A sample every SAMPLE_MS on the way, each where the finger would be by then. (Once one sleep of a
 * frame after another: those came out at 21 ms apart, 48 samples a second, fewer than the display's
 * 60 frames, and an application that is moved by the samples drew no more frames than that.)
 */
static void
drag(double x0, double y0, double x1, double y1, int ms, int hold, int grab)
{
	uint64_t start, total_us = (uint64_t)(ms > 0 ? ms : 1) * 1000;

	post(REDWOOD_TOUCH_DOWN, x0, y0);
	/*
	 * grab: stay where the finger landed before moving at all. An icon of the home screen is
	 * picked up by a long press and only then dragged, and it has to be one touch throughout --
	 * a press that ends puts up the icon's menu instead.
	 */
	for (int waited = 0; waited < grab; waited += 100) {
		wait_us(100000);
		post(REDWOOD_TOUCH_MOVE, x0, y0);
	}
	start = mach_absolute_time();
	for (uint64_t at = SAMPLE_MS * 1000;; at += SAMPLE_MS * 1000) {
		double part;

		if (at > total_us) {
			at = total_us;
		}
		part = (double)at / (double)total_us;
		sleep_until(start, at);
		post(REDWOOD_TOUCH_MOVE, x0 + (x1 - x0) * part, y0 + (y1 - y0) * part);
		if (at == total_us) {
			break;
		}
	}
	wait_us(16000);
	if (hold > 0) {
		/* (a sample now and then, as a finger that stays still still gives) */
		for (int waited = 0; waited < hold; waited += 100) {
			wait_us(100000);
			post(REDWOOD_TOUCH_MOVE, x1, y1);
		}
	}
	post(REDWOOD_TOUCH_UP, x1, y1);
}

static void
zigzag(double x, double y0, double y1, int ms, int times, int step_ms)
{
	uint64_t start, legs = (uint64_t)times * 2, total_us = legs * (uint64_t)ms * 1000;

	post(REDWOOD_TOUCH_DOWN, x, y0);
	start = mach_absolute_time();
	for (uint64_t at = (uint64_t)step_ms * 1000; at <= total_us; at += (uint64_t)step_ms * 1000) {
		uint64_t leg = at / ((uint64_t)ms * 1000), within = at % ((uint64_t)ms * 1000);
		double part = (double)within / (ms * 1000.0), y;

		if (leg >= legs) {
			leg = legs - 1;
			part = 1;
		}
		y = leg % 2 == 0 ? y0 + (y1 - y0) * part : y1 + (y0 - y1) * part;
		sleep_until(start, at);
		post(REDWOOD_TOUCH_MOVE, x, y);
	}
	wait_us(16000);
	post(REDWOOD_TOUCH_UP, x, y0);
}

int
main(int argc, char **argv)
{
	/* a gesture is a matter of timing: do not let the rest of a busy system stretch it */
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	mach_timebase_info(&timebase);
	device = open(REDWOOD_TOUCH_DEVICE, O_RDWR);
	if (device < 0) {
		perror("rwtouch: " REDWOOD_TOUCH_DEVICE);
		return 1;
	}
	trace("rwtouch: %d step%s\n", argc - 1, argc == 2 ? "" : "s");
	for (int i = 1; i < argc; i++) {
		const char *step = argv[i];
		int left = argc - i - 1;

		if (strcmp(step, "wait") == 0 && left >= 1) {
			wait_us((uint64_t)(atof(argv[i + 1]) * 1e6));
			i += 1;
		} else if (strcmp(step, "tap") == 0 && left >= 2) {
			double x = atof(argv[i + 1]), y = atof(argv[i + 2]);

			post(REDWOOD_TOUCH_DOWN, x, y);
			wait_us(60000);
			post(REDWOOD_TOUCH_UP, x, y);
			i += 2;
		} else if (strcmp(step, "zigzag") == 0 && left >= 5) {
			int step_ms = left >= 6 && atoi(argv[i + 6]) > 0 ? atoi(argv[i + 6]) : 8;

			zigzag(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atoi(argv[i + 4]), atoi(argv[i + 5]), step_ms);
			i += left >= 6 && atoi(argv[i + 6]) > 0 ? 6 : 5;
		} else if (strcmp(step, "hold") == 0 && left >= 3) {
			double x = atof(argv[i + 1]), y = atof(argv[i + 2]);

			post(REDWOOD_TOUCH_DOWN, x, y);
			wait_us((uint64_t)atoi(argv[i + 3]) * 1000);
			post(REDWOOD_TOUCH_UP, x, y);
			i += 3;
		} else if (strcmp(step, "button") == 0 && left >= 1) {
			const char *name = argv[i + 1];
			uint32_t which = strcmp(name, "home") == 0 ? REDWOOD_BUTTON_HOME : strcmp(name, "lock") == 0 ?
			    REDWOOD_BUTTON_LOCK : strcmp(name, "volup") == 0 ? REDWOOD_BUTTON_VOLUME_UP :
			    strcmp(name, "voldown") == 0 ? REDWOOD_BUTTON_VOLUME_DOWN : 0;
			int ms = left >= 2 && argv[i + 2][0] >= '0' && argv[i + 2][0] <= '9' ? atoi(argv[i + 2]) : 0;

			if (which == 0) {
				fprintf(stderr, "rwtouch: buttons are home, lock, volup and voldown\n");
				return 2;
			}
			button(which, ms ? ms : 80);
			i += ms ? 2 : 1;
		} else if (strcmp(step, "swipe") == 0 && left >= 4) {
			int ms = left >= 5 && argv[i + 5][0] >= '0' && argv[i + 5][0] <= '9' ? atoi(argv[i + 5]) : 0;

			drag(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4]), ms ? ms : 300, 0, 0);
			i += ms ? 5 : 4;
		} else if (strcmp(step, "drag") == 0 && left >= 6) {
			int grab = left >= 7 && argv[i + 7][0] >= '0' && argv[i + 7][0] <= '9' ? atoi(argv[i + 7]) : 0;

			drag(atof(argv[i + 1]), atof(argv[i + 2]), atof(argv[i + 3]), atof(argv[i + 4]), atoi(argv[i + 5]),
			    atoi(argv[i + 6]), grab);
			i += grab ? 7 : 6;
		} else if (strcmp(step, "notify") == 0 && left >= 1) {
			uint32_t status = notify_post(argv[i + 1]);

			printf("rwtouch: notification %s: %s\n", argv[i + 1], status == NOTIFY_STATUS_OK ? "posted" : "FAILED");
			i += 1;
		} else if (strcmp(step, "heartbeat") == 0 && left >= 2) {
			/* "heartbeat SECONDS EACH": a line every EACH seconds for SECONDS on the CPU's counter.
			 * It is how a run tells a program that is waiting from one that is stuck: the line says
			 * the counter itself, which no sleep can stretch. */
			double total = atof(argv[i + 1]), each = atof(argv[i + 2]);
			uint64_t start = now_us(), until = start + (uint64_t)(total * 1e6);

			do {
				trace("rwtouch: heartbeat %llu us (waited %llu)\n",
				    (unsigned long long)now_us(), (unsigned long long)(now_us() - start));
				if (now_us() < until) {
					wait_us((uint64_t)(each * 1e6));
				}
			} while (now_us() < until);
			i += 2;
		} else if (strcmp(step, "clock") == 0) {
			/* one sleep of each length, said as soon as it returns: which of them never returns */
			static const useconds_t ladder[] = { 1000, 2000, 4000, 8000, 12000, 16000, 24000, 33000, 50000 };
			uint64_t start, took;

			trace("rwtouch: timebase %u/%u\n", timebase.numer, timebase.denom);
			for (unsigned k = 0; k < sizeof(ladder) / sizeof(ladder[0]); k++) {
				start = now_us();
				usleep(ladder[k]);
				took = now_us() - start;
				trace("rwtouch: usleep(%u) took %llu us\n", ladder[k], (unsigned long long)took);
			}
			/* the same again as a wait, which is what a gesture of the list uses */
			trace("rwtouch: a wait of 1000000 us begins\n");
			start = now_us();
			wait_us(1000000);
			took = now_us() - start;
			trace("rwtouch: a wait of 1000000 us took %llu us\n", (unsigned long long)took);
		} else if (strcmp(step, "read") == 0) {
			struct redwood_touch t;

			while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
				if (t.type >= REDWOOD_BUTTON_DOWN) {
					printf("rwtouch: button %u %s\n", t.finger, t.type == REDWOOD_BUTTON_DOWN ? "down" : "up");
					continue;
				}
				printf("rwtouch: %s finger %u at %.3f %.3f\n", t.type == REDWOOD_TOUCH_DOWN ? "down" :
				    t.type == REDWOOD_TOUCH_MOVE ? "move" : "up  ", t.finger, t.x / 65535.0, t.y / 65535.0);
			}
			return 0;
		} else {
			fprintf(stderr, "rwtouch: bad step \"%s\"\n", step);
			return 2;
		}
		wait_us(50000);
	}
	trace("rwtouch: done\n");
	return 0;
}
