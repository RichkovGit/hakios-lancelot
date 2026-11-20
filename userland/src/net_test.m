/*
 * net_test: bring the network interface up the way iOS itself would, and see whether iOS can
 * use it. Each step is one layer of what a stock application needs:
 *
 * usage: net_test STEP...
 *   show                  the interfaces and their addresses, as the kernel has them
 *   up IF                 ask the kernel to bring IF up (IFF_UP), and show it
 *   lookup NAME...        whether launchd has that Mach service (bootstrap_look_up)
 *   procs WORD            the processes whose path has WORD in it (whether a job is running)
 *   me                    the process: its number, user and group (which domain it was started in)
 *   dir PATH              what is in that directory (and whether PATH/wifid.plist-like names are there)
 *   radio on|off          the radio's power, as iOS's Wi-Fi side sets it (leave, and rejoin on)
 *   fbwait SECONDS        wait until the panel has had a frame (kern.redwood.fbgrab says EINVAL
 *                         until the first swap: on this kernel the first one comes ~80 s in)
 *   end                   end the run here (kern.redwood.end): the log keeps the last ~261 KB, and
 *                         anything said a second or two before the run's own end is lost
 *   stop NAME / cont NAME freeze / resume the processes whose path has NAME (wifid, so that the radio
 *                         stays off when it is asked to)
 *   waitproc NAME SECONDS wait for a program named NAME to appear (readiness gate before tapping)
 *   watchdie NAME SECONDS
 *                         wait for it to appear and then to go (a program that aborts); the "end"
 *                         after it keeps the reason applog=1 printed on the console
 *   store [PATTERN]       what configd has in its dynamic store (default: everything under
 *                         State:/Network and Setup:/Network)
 *   dhcp IF               ask configd's IPConfiguration plugin to configure IF by DHCP
 *   waitaddr IF SECONDS   wait until IF has an IPv4 address
 *   resolve NAME          getaddrinfo(): the question goes to mDNSResponder, which asks the
 *                         servers configd has told it about
 *   tcp HOST PORT         connect a socket, send an HTTP request, show the first line of the answer
 *   fetch URL [SECONDS]   the same through CFNetwork (NSURLSession): for https this is TLS, the
 *                         system's trust store and trustd as well (default 60 s: a run with the radio
 *                         off wants a shorter one)
 *   reach NAME            what SCNetworkReachability says: the question applications ask first
 *   wait SECONDS
 *   wifi                  what the job com.redwood.wifi runs in every image: if the kernel was
 *                         started with wifi=1, wait for the radio's interface (en0) to appear,
 *                         and ask for it to be configured by DHCP ("dhcp en0") only if configd
 *                         has no network service of its own for it; else nothing
 *   watch SECONDS         for so long, say when configd's store changes what tells an interface's
 *                         state: State:/Network/Service/.../IPv4, State:/Network/Interface/en0/...,
 *                         State:/Network/Global/IPv4 (the key, whether it is there, the names in it;
 *                         no addresses)
 *   wrongjoin             ask the radio to join the network it is on with a passphrase that is
 *                         wrong on purpose, through the request wifid uses (Apple80211's
 *                         ASSOCIATE), and say what the kernel answers: Apple80211's result number
 *                         (21: a wrong passphrase) and how long it took. The network's name is
 *                         the kernel's (its SSID request) and is not printed.
 *
 * The interface QEMU's network gives (en0, bsd/dev/redwood_net.c) has no IOKit object behind it,
 * and configd makes its network services out of those. (The phone's radio has one now:
 * iokit/Kernel/RedwoodWLAN.cpp, and configd makes a "Wi-Fi" service of it by itself.) But the
 * plugin that does the configuring keeps its own list
 * of interfaces, straight from the kernel, and has a Mach interface of its own, the one the
 * "ipconfig" command of macOS uses ("ipconfig set en0 DHCP"; the command itself is not part of
 * iOS). "dhcp" is one of its calls: the plugin makes a service for the interface that exists in
 * no preferences file, runs DHCP, and publishes what it learns; the rest of configd then treats
 * it like any other service.
 * (Not "ipconfig set", the first call one thinks of: the service that one makes is gone at
 * configd's next look at its configuration, half a second later at boot, and the protocol is
 * detached from the interface again. "ipconfig setservice" makes one that stays.)
 */
#import <Foundation/Foundation.h>
#include <SystemConfiguration/SCNetworkReachability.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <ifaddrs.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* not in the iOS SDK's headers, in the libraries all the same */
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *service);
typedef const struct __SCDynamicStore *SCDynamicStoreRef;
extern SCDynamicStoreRef SCDynamicStoreCreate(CFAllocatorRef allocator, CFStringRef name, void *callout, void *context);
extern CFArrayRef SCDynamicStoreCopyKeyList(SCDynamicStoreRef store, CFStringRef pattern);
extern CFPropertyListRef SCDynamicStoreCopyValue(SCDynamicStoreRef store, CFStringRef key);
extern Boolean SCDynamicStoreSetNotificationKeys(SCDynamicStoreRef store, CFArrayRef keys, CFArrayRef patterns);
extern CFRunLoopSourceRef SCDynamicStoreCreateRunLoopSource(CFAllocatorRef allocator, SCDynamicStoreRef store, CFIndex order);
typedef const struct __SCPreferences *SCPreferencesRef;
extern SCPreferencesRef SCPreferencesCreate(CFAllocatorRef allocator, CFStringRef name, CFStringRef prefsID);
extern CFPropertyListRef SCPreferencesGetValue(SCPreferencesRef prefs, CFStringRef key);
extern void SCPreferencesSynchronize(SCPreferencesRef prefs);
extern Boolean SCPreferencesLock(SCPreferencesRef prefs, Boolean wait);
extern Boolean SCPreferencesUnlock(SCPreferencesRef prefs);
extern Boolean SCPreferencesSetComputerName(SCPreferencesRef prefs, CFStringRef name, CFStringEncoding encoding);
extern Boolean SCPreferencesCommitChanges(SCPreferencesRef prefs);
extern Boolean SCPreferencesApplyChanges(SCPreferencesRef prefs);
extern CFStringRef SCDynamicStoreCopyComputerName(SCDynamicStoreRef store, CFStringEncoding *encoding);
extern int SCError(void);
extern const char *SCErrorString(int status);

static double
seconds(void)
{
	static mach_timebase_info_data_t base;

	if (base.denom == 0) {
		mach_timebase_info(&base);
	}
	return (double)mach_absolute_time() * base.numer / base.denom / 1e9;
}

/*
 * Every line goes to the console and, whole, into kern.redwood.host_keep (bsd/dev/redwood_mali.c):
 * the console the host gets back from a run is its first 8 KB and its last ~250 KB, and a job that
 * says what it found before the last of those is lost with the middle (a widget's lookup failures
 * filled a run's tail: run 71). host_keep is put into ramoops' pmsg zone, 64 KB, and appended whole.
 */
static char note_line[1024];
static size_t note_length;

static void
note(const char *format, ...) __printflike(1, 2);
static void
note(const char *format, ...)
{
	char text[1024];
	va_list ap;
	int length;

	va_start(ap, format);
	length = vsnprintf(text, sizeof(text), format, ap);
	va_end(ap);
	if (length <= 0) {
		return;
	}
	fwrite(text, 1, (size_t)length, stdout);
	for (int i = 0; i < length; i++) {
		if (text[i] == '\n') {
			sysctlbyname("kern.redwood.host_keep", NULL, NULL, note_line, note_length);
			note_length = 0;
			continue;
		}
		if (note_length == 0) {
			note_length = (size_t)snprintf(note_line, sizeof(note_line), "[%.2f] ", seconds());
		}
		if (note_length < sizeof(note_line)) {
			note_line[note_length++] = text[i];
		}
	}
	fflush(stdout);
}
#define printf note

/* ---- show ------------------------------------------------------------------------------------------ */

static bool show(const char *only, bool quiet);

/* ---- up ------------------------------------------------------------------------------------------- */

/* The stack sends nothing on an interface that is not up: the kernel marks it up when an address is
 * set (SIOCSIFADDR) or when it is asked to (SIOCSIFFLAGS). iOS's configd does it as it configures a
 * service; with configd not running (run 72), this is what is left to see the driver's side work. */
static void
up(const char *interface)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifreq request;
	int error;

	if (s < 0) {
		printf("net_test: up %s: no socket: %s\n", interface, strerror(errno));
		return;
	}
	memset(&request, 0, sizeof(request));
	strlcpy(request.ifr_name, interface, sizeof(request.ifr_name));
	if (ioctl(s, SIOCGIFFLAGS, &request) != 0) {
		printf("net_test: up %s: the flags cannot be read: %s\n", interface, strerror(errno));
		close(s);
		return;
	}
	request.ifr_flags |= IFF_UP;
	error = ioctl(s, SIOCSIFFLAGS, &request);
	close(s);
	if (error != 0) {
		printf("net_test: up %s: the kernel refuses: %s\n", interface, strerror(errno));
		return;
	}
	printf("net_test: up %s: asked, flags %#x\n", interface, request.ifr_flags);
	show(interface, false);
}

/* ---- lookup ---------------------------------------------------------------------------------------- */

/* libSystem's, without the header the iOS SDK leaves out (userland/src/psgrep.c) */
int proc_pidpath(int pid, void *buffer, uint32_t size);

/* ---- procs ---------------------------------------------------------------------------------------- */

/* What is in a directory: which plists launchd is looking at (the image's own, or the iOS volume's). */
static void
directory(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *entry;
	unsigned count = 0;
	bool wifid = false, configd = false, demo = false;

	if (d == NULL) {
		printf("net_test: dir %s: %s\n", path, strerror(errno));
		return;
	}
	while ((entry = readdir(d)) != NULL) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		count++;
		wifid |= strcmp(entry->d_name, "com.apple.wifid.plist") == 0;
		configd |= strcmp(entry->d_name, "com.apple.configd.plist") == 0;
		demo |= strcmp(entry->d_name, "com.redwood.demo.plist") == 0;
	}
	closedir(d);
	printf("net_test: dir %s: %u entries, wifid %s, configd %s, demo %s\n", path, count, wifid ? "there" : "NOT there",
	    configd ? "there" : "NOT there", demo ? "there" : "NOT there");
}

/* The processes whose path has WORD in it, case ignored: whether a job launchd should have started
 * is running at all (configd, wifid, mDNSResponder...). */
static void
procs(const char *word)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0, count;
	struct kinfo_proc *list;
	char path[1024];
	unsigned found = 0;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || (list = malloc(size + 16 * sizeof(*list))) == NULL ||
	    sysctl(mib, 3, list, &size, NULL, 0) != 0) {
		printf("net_test: procs: the process list cannot be read: %s\n", strerror(errno));
		return;
	}
	count = size / sizeof(*list);
	for (size_t n = 0; n < count; n++) {
		path[0] = 0;
		if (proc_pidpath(list[n].kp_proc.p_pid, path, sizeof(path)) <= 0) {
			continue;
		}
		if (strcasestr(path, word) != NULL) {
			printf("net_test: proc %d %s\n", list[n].kp_proc.p_pid, path);
			found++;
		}
	}
	printf("net_test: procs: %u of %lu processes have \"%s\"\n", found, (unsigned long)count, word);
	free(list);
}

/*
 * Whether PATH is a program named WORD: the last part of the path only. A whole-path match finds
 * things that are not it -- "Preferences" found IntlPreferences.framework/Support/localizationswitcherd
 * in run 91, so watchdie never saw Settings go.
 */
static bool
matches(const char *path, const char *word)
{
	const char *base = strrchr(path, '/');

	return strcasestr(base != NULL ? base + 1 : path, word) != NULL;
}

/*
 * The pids of the programs named WORD. Returns how many were put in PIDS (at most MAX).
 */
static unsigned
named_pids(const char *word, pid_t *pids, unsigned max)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
	size_t size = 0, count;
	struct kinfo_proc *list;
	char path[1024];
	unsigned found = 0;

	if (sysctl(mib, 3, NULL, &size, NULL, 0) != 0 || (list = malloc(size + 16 * sizeof(*list))) == NULL ||
	    sysctl(mib, 3, list, &size, NULL, 0) != 0) {
		return 0;
	}
	count = size / sizeof(*list);
	for (size_t n = 0; n < count && found < max; n++) {
		path[0] = 0;
		if (proc_pidpath(list[n].kp_proc.p_pid, path, sizeof(path)) <= 0) {
			continue;
		}
		if (matches(path, word)) {
			pids[found++] = list[n].kp_proc.p_pid;
		}
	}
	free(list);
	return found;
}

/*
 * stop NAME / cont NAME: SIGSTOP / SIGCONT on every process named NAME.
 *
 * The point is wifid. The radio's power belongs to it (run 81: a program's "radio off" was answered
 * with "power, 1" by wifid 17 ms later, and the network was back in 194 ms), because Settings' switch
 * does not ask the kernel: wifid sets power 0 and remembers that. A stopped wifid is not a crashed
 * one, so launchd (KeepAlive) does not start another, and the power a program asks for stays asked
 * for -- which is what the switch's own off state is to the driver. SIGCONT at the end gives it back.
 */
static void
signal_named(const char *word, int what, const char *said)
{
	pid_t pids[16];
	unsigned found = named_pids(word, pids, 16);

	if (found == 0) {
		printf("net_test: %s %s: no such process\n", said, word);
		return;
	}
	for (unsigned n = 0; n < found; n++) {
		char path[1024] = "?";
		int error;

		proc_pidpath(pids[n], path, sizeof(path));
		error = kill(pids[n], what);
		printf("net_test: %s %s: %d %s: %s\n", said, word, pids[n], path, error == 0 ? "done" : strerror(errno));
	}
}

/*
 * watchdie NAME SECONDS: wait for a process whose path has NAME to appear, then wait for it to go,
 * and say how long each took. The reason is the end of the run: a program that aborts says why on its
 * own standard error, and boot-arg applog=1 puts that on the console (bsd/kern/kern_exec.c); but the
 * log the host gets keeps only the console's last ~261 KB, which the widgets fill in one or two
 * seconds. Ending the run at the death itself (the "end" step, in the same command line) keeps the
 * text of the abort, which is what Preferences has not told us yet.
 */
static void
watchdie(const char *word, double timeout)
{
	double began = seconds(), appeared = 0;
	pid_t pids[16];
	unsigned found = 0;

	while (seconds() - began < timeout) {
		if ((found = named_pids(word, pids, 16)) > 0) {
			appeared = seconds();
			break;
		}
		usleep(20000);
	}
	if (appeared == 0) {
		printf("net_test: watchdie %s: it did not appear in %.1f s\n", word, seconds() - began);
		return;
	}
	printf("net_test: watchdie %s: %d appears after %.2f s\n", word, pids[0], appeared - began);
	while (seconds() - appeared < timeout) {
		if (named_pids(word, pids, 16) == 0) {
			printf("net_test: watchdie %s: gone after %.2f s (it was there for %.2f s)\n", word,
			    seconds() - began, seconds() - appeared);
			return;
		}
		usleep(20000);
	}
	printf("net_test: watchdie %s: still there after %.1f s\n", word, seconds() - appeared);
}

/*
 * waitproc NAME SECONDS: wait for a program named NAME to appear (the last part of its path, as
 * named_pids matches), and say its pid and how long it took. The native-render runs (72, 73) tapped
 * the screen before SpringBoard had started, because fbwait returned at once on an early frame; this
 * is the readiness gate they lacked: wait for SpringBoard itself, then let the compositor settle.
 */
static void
waitproc(const char *word, double timeout)
{
	double began = seconds();
	pid_t pids[16];
	unsigned found;

	while (seconds() - began < timeout) {
		if ((found = named_pids(word, pids, 16)) > 0) {
			printf("net_test: waitproc %s: %d appears after %.2f s\n", word, pids[0], seconds() - began);
			return;
		}
		usleep(50000);
	}
	printf("net_test: waitproc %s: did not appear in %.1f s\n", word, seconds() - began);
}

static void
lookup(const char *name)
{
	mach_port_t service = MACH_PORT_NULL, task_bootstrap = MACH_PORT_NULL;
	kern_return_t result = bootstrap_look_up(bootstrap_port, name, &service);

	task_get_bootstrap_port(mach_task_self(), &task_bootstrap);
	if (result == KERN_SUCCESS) {
		printf("net_test: lookup %s: there (bootstrap_port %#x, the task's %#x)\n", name, bootstrap_port, task_bootstrap);
		mach_port_deallocate(mach_task_self(), service);
	} else if (task_bootstrap != bootstrap_port && task_bootstrap != MACH_PORT_NULL &&
	    bootstrap_look_up(task_bootstrap, name, &service) == KERN_SUCCESS) {
		printf("net_test: lookup %s: there through the task's bootstrap port %#x, not the global %#x\n", name,
		    task_bootstrap, bootstrap_port);
		mach_port_deallocate(mach_task_self(), service);
	} else {
		printf("net_test: lookup %s: %s (%#x) (bootstrap_port %#x, the task's %#x)\n", name, mach_error_string(result),
		    result, bootstrap_port, task_bootstrap);
	}
}

static bool
show(const char *only, bool quiet)
{
	struct ifaddrs *list, *a;
	bool has_ipv4 = false;
	char text[64];

	if (getifaddrs(&list) != 0) {
		perror("net_test: getifaddrs");
		return false;
	}
	for (a = list; a != NULL; a = a->ifa_next) {
		if (a->ifa_addr == NULL || (only != NULL && strcmp(a->ifa_name, only) != 0)) {
			continue;
		}
		if (a->ifa_addr->sa_family == AF_LINK) {
			struct sockaddr_dl *dl = (struct sockaddr_dl *)a->ifa_addr;
			const unsigned char *m = (const unsigned char *)LLADDR(dl);

			if (quiet) {
				continue;
			}
			printf("net_test: %s: flags %#x%s%s, type %#x", a->ifa_name, a->ifa_flags, (a->ifa_flags & IFF_UP) ? " up" : "",
			    (a->ifa_flags & IFF_RUNNING) ? " running" : "", dl->sdl_type);
			if (dl->sdl_alen == 6) {
				printf(", address %02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
			}
			printf("\n");
		} else if (a->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in *in = (struct sockaddr_in *)a->ifa_addr, *mask = (struct sockaddr_in *)a->ifa_netmask;

			has_ipv4 = true;
			if (!quiet) {
				printf("net_test: %s: inet %s", a->ifa_name, inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)));
				printf(" netmask %s\n", mask ? inet_ntop(AF_INET, &mask->sin_addr, text, sizeof(text)) : "?");
			}
		} else if (a->ifa_addr->sa_family == AF_INET6 && !quiet) {
			struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)a->ifa_addr;

			printf("net_test: %s: inet6 %s\n", a->ifa_name, inet_ntop(AF_INET6, &in6->sin6_addr, text, sizeof(text)));
		}
	}
	freeifaddrs(list);
	return has_ipv4;
}

/* ---- store ----------------------------------------------------------------------------------------- */

static void
store(const char *pattern)
{
	SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test"), NULL, NULL);
	NSString *wanted = pattern ? @(pattern) : @"(State|Setup):/Network/.*";
	NSArray *keys;

	if (session == NULL) {
		printf("net_test: store: no session with configd\n");
		return;
	}
	keys = CFBridgingRelease(SCDynamicStoreCopyKeyList(session, (__bridge CFStringRef)wanted));
	printf("net_test: store: %lu keys match %s\n", (unsigned long)keys.count, wanted.UTF8String);
	for (NSString *key in [keys sortedArrayUsingSelector:@selector(compare:)]) {
		id value = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));
		NSString *text = [[[value description] componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
		    componentsJoinedByString:@" "];

		while ([text containsString:@"  "]) {
			text = [text stringByReplacingOccurrencesOfString:@"  " withString:@" "];
		}
		printf("net_test:   %s = %s\n", key.UTF8String, text.length > 400 ? [text substringToIndex:400].UTF8String : text.UTF8String);
	}
	CFRelease(session);
}

/* ---- dhcp ------------------------------------------------------------------------------------------ */

/*
 * The one call of the plugin's interface that is needed, written out by hand (bootp's
 * ipconfig.defs, subsystem 20000, the twelfth routine):
 *   routine ipconfig_set_service(server; name : array[16] of char; method_data : ^array[] of byte;
 *                                out serviceID : array[128] of char; out status : int)
 * method_data is a property list, { ConfigMethod = DHCP }, sent out of line. Only root may ask.
 */
#pragma pack(push, 4)
struct set_request {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_ool_descriptor_t method;
	NDR_record_t ndr;
	char name[16];
	mach_msg_type_number_t method_length;
};
struct set_reply {
	mach_msg_header_t head;
	NDR_record_t ndr;
	kern_return_t result;
	char service[128];
	int status;
	mach_msg_max_trailer_t trailer;
};
#pragma pack(pop)

static void
dhcp(const char *interface)
{
	static const char *const statuses[] = { "done", "permission denied", "no such interface", "invalid parameter",
		                                "invalid operation", "allocation failed", "internal error", "not supported" };
	NSData *method = [NSPropertyListSerialization dataWithPropertyList:@{ @"ConfigMethod": @"DHCP" }
	    format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
	mach_port_t server = MACH_PORT_NULL, reply_port = mig_get_reply_port();
	union {
		struct set_request request;
		struct set_reply reply;
	} message;
	kern_return_t result;

	result = bootstrap_look_up(bootstrap_port, "com.apple.network.IPConfiguration", &server);
	if (result != KERN_SUCCESS) {
		printf("net_test: dhcp: configd's IPConfiguration is not there: %#x\n", result);
		return;
	}
	printf("net_test: dhcp: asking configd's IPConfiguration to configure \"%s\" by DHCP\n", interface);
	memset(&message, 0, sizeof(message));
	message.request.head.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
	message.request.head.msgh_size = sizeof(message.request);
	message.request.head.msgh_remote_port = server;
	message.request.head.msgh_local_port = reply_port;
	message.request.head.msgh_id = 20011;
	message.request.body.msgh_descriptor_count = 1;
	message.request.method.address = (void *)method.bytes;
	message.request.method.size = (mach_msg_size_t)method.length;
	message.request.method.deallocate = FALSE;
	message.request.method.copy = MACH_MSG_VIRTUAL_COPY;
	message.request.method.type = MACH_MSG_OOL_DESCRIPTOR;
	message.request.ndr = NDR_record;
	strlcpy(message.request.name, interface, sizeof(message.request.name));
	message.request.method_length = (mach_msg_type_number_t)method.length;
	result = mach_msg(&message.request.head, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_TIMEOUT, sizeof(message.request), sizeof(message),
	    reply_port, 10000, MACH_PORT_NULL);
	if (result == MACH_RCV_TIMED_OUT) {
		printf("net_test: dhcp: no answer from IPConfiguration in 10 s (the call is not answered at all)\n");
	} else if (result != KERN_SUCCESS) {
		printf("net_test: dhcp: the message did not go through: %#x\n", result);
	} else if (message.reply.result != KERN_SUCCESS) {
		printf("net_test: dhcp: IPConfiguration does not take the message: %d\n", message.reply.result);
	} else {
		int status = message.reply.status;

		message.reply.service[sizeof(message.reply.service) - 1] = 0;
		printf("net_test: dhcp: IPConfiguration says of \"setservice %s DHCP\": %d (%s), service %s\n", interface, status,
		    status >= 0 && status < 8 ? statuses[status] : "?", message.reply.service);
	}
}

/* ---- resolve, tcp ---------------------------------------------------------------------------------- */

static bool
resolve(const char *name, char *first, size_t size)
{
	struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *list = NULL, *a;
	double start = seconds();
	int error = getaddrinfo(name, NULL, &hints, &list);
	char text[64];

	if (error != 0) {
		printf("net_test: resolve %s: %s (after %.2f s)\n", name, gai_strerror(error), seconds() - start);
		return false;
	}
	printf("net_test: resolve %s, %.2f s:", name, seconds() - start);
	for (a = list; a != NULL; a = a->ai_next) {
		void *address = a->ai_family == AF_INET ? (void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr :
		    (void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;

		printf(" %s", inet_ntop(a->ai_family, address, text, sizeof(text)));
		if (a->ai_family == AF_INET && first != NULL && first[0] == 0) {
			strlcpy(first, text, size);
		}
	}
	printf("\n");
	freeaddrinfo(list);
	return true;
}

static void
tcp(const char *host, const char *port)
{
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *list = NULL;
	char request[256], answer[2048], *end;
	double start = seconds();
	ssize_t n, total = 0;
	int s, error;

	error = getaddrinfo(host, port, &hints, &list);
	if (error != 0) {
		printf("net_test: tcp %s: %s\n", host, gai_strerror(error));
		return;
	}
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0 || connect(s, list->ai_addr, list->ai_addrlen) != 0) {
		printf("net_test: tcp %s port %s: %s (after %.2f s)\n", host, port, strerror(errno), seconds() - start);
		freeaddrinfo(list);
		return;
	}
	printf("net_test: tcp %s port %s: connected in %.2f s\n", host, port, seconds() - start);
	snprintf(request, sizeof(request), "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: net_test\r\n\r\n", host);
	write(s, request, strlen(request));
	answer[0] = 0;
	while ((n = read(s, total == 0 ? answer : request, total == 0 ? sizeof(answer) - 1 : sizeof(request))) > 0) {
		if (total == 0) {
			answer[n] = 0;
		}
		total += n;
	}
	if ((end = strpbrk(answer, "\r\n")) != NULL) {
		*end = 0;
	}
	printf("net_test: tcp %s: %zd bytes in %.2f s, the answer begins \"%s\"\n", host, total, seconds() - start, answer);
	close(s);
	freeaddrinfo(list);
}

/* ---- fetch, reach ---------------------------------------------------------------------------------- */

static void
fetch(const char *address, double limit)
{
	NSURL *url = [NSURL URLWithString:@(address)];
	NSURLSessionConfiguration *configuration = NSURLSessionConfiguration.ephemeralSessionConfiguration;
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	double start = seconds();

	configuration.timeoutIntervalForRequest = limit;
	[[[NSURLSession sessionWithConfiguration:configuration] dataTaskWithURL:url
	    completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
		if (error != nil) {
			printf("net_test: fetch %s: FAILED after %.2f s: %s (%s %ld)\n", address, seconds() - start,
			    error.localizedDescription.UTF8String, error.domain.UTF8String, (long)error.code);
		} else {
			NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
			NSRange title = [text rangeOfString:@"<title>"], close = [text rangeOfString:@"</title>"];

			printf("net_test: fetch %s: status %ld, %lu bytes in %.2f s", address,
			    (long)((NSHTTPURLResponse *)response).statusCode, (unsigned long)data.length, seconds() - start);
			if (title.location != NSNotFound && close.location != NSNotFound && close.location > title.location) {
				printf(", title \"%s\"", [text substringWithRange:NSMakeRange(NSMaxRange(title),
				    close.location - NSMaxRange(title))].UTF8String);
			}
			printf("\n");
		}
		dispatch_semaphore_signal(done);
	}] resume];
	if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(limit + 5) * NSEC_PER_SEC)) != 0) {
		printf("net_test: fetch %s: no answer and no error in %.0f s\n", address, limit + 5);
	}
}

static void
reach(const char *name)
{
	SCNetworkReachabilityRef target = SCNetworkReachabilityCreateWithName(NULL, name);
	SCNetworkReachabilityFlags flags = 0;

	if (target == NULL || !SCNetworkReachabilityGetFlags(target, &flags)) {
		printf("net_test: reach %s: no answer\n", name);
	} else {
		printf("net_test: reach %s: flags %#x:%s%s%s%s\n", name, flags, (flags & kSCNetworkReachabilityFlagsReachable) ?
		    " reachable" : " NOT reachable", (flags & kSCNetworkReachabilityFlagsConnectionRequired) ? ", connection required" : "",
		    (flags & kSCNetworkReachabilityFlagsIsWWAN) ? ", cellular" : "",
		    (flags & kSCNetworkReachabilityFlagsIsDirect) ? ", direct" : "");
	}
	if (target != NULL) {
		CFRelease(target);
	}
}

/*
 * The device's name, which a restored iPhone has from its set-up and this one has not: Settings'
 * Bluetooth pane wrote "discoverable as (null)" (BluetoothSettings asks SCDynamicStoreCopyComputerName,
 * run 760) and bluetoothd named the radio "iOS". Written the way iOS writes it, into the system
 * configuration's preferences, and only after configd has made its own (a set of network services):
 * preferences of ours with nothing but a name in them would be taken for a configuration, and the
 * Wi-Fi's service would not be made. Boot-arg devname=NAME names it otherwise (no spaces); "iPhone"
 * is what MobileGestalt answers here.
 */
static void
device_name(void)
{
	char arguments[1024] = "", chosen[64] = "iPhone";
	size_t size = sizeof(arguments) - 1;
	double start = seconds();
	SCPreferencesRef preferences = NULL;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (const char *word = strtok(arguments, " "); word != NULL; word = strtok(NULL, " ")) {
			if (strncmp(word, "devname=", 8) == 0 && word[8] != 0) {
				snprintf(chosen, sizeof(chosen), "%s", word + 8);
			}
		}
	}
	for (;;) {
		CFStringRef has = SCDynamicStoreCopyComputerName(NULL, NULL);

		if (has != NULL) {
			printf("net_test: name: the device is already named \"%s\"\n", ((__bridge NSString *)has).UTF8String);
			CFRelease(has);
			return;
		}
		if (preferences != NULL) {
			CFRelease(preferences);
		}
		preferences = SCPreferencesCreate(NULL, CFSTR("net_test"), NULL);
		if (preferences != NULL && SCPreferencesGetValue(preferences, CFSTR("CurrentSet")) != NULL) {
			break;
		}
		if (seconds() - start > 90) {
			printf("net_test: name: configd has made no configuration in 90 s; the name is left unset\n");
			return;
		}
		sleep(1);
	}
	CFStringRef name = CFStringCreateWithCString(NULL, chosen, kCFStringEncodingUTF8);
	bool locked = SCPreferencesLock(preferences, true);
	bool done = name != NULL && SCPreferencesSetComputerName(preferences, name, kCFStringEncodingUTF8) &&
	    SCPreferencesCommitChanges(preferences) && SCPreferencesApplyChanges(preferences);
	int error = done ? 0 : SCError();

	if (locked) {
		SCPreferencesUnlock(preferences);
	}
	if (!done) {
		printf("net_test: name: \"%s\" not set: %s\n", chosen, SCErrorString(error));
		return;
	}
	for (int i = 0; i < 20; i++) {
		CFStringRef has = SCDynamicStoreCopyComputerName(NULL, NULL);

		if (has != NULL) {
			printf("net_test: name: the device is named \"%s\" after %.1f s\n", ((__bridge NSString *)has).UTF8String, seconds() - start);
			CFRelease(has);
			return;
		}
		usleep(250000);
	}
	printf("net_test: name: \"%s\" written, configd does not say it yet\n", chosen);
}

/*
 * With wifi=1 among the kernel's arguments: wait for en0, and for configd's own network service for
 * it ("Wi-Fi", made of the radio's IOKit objects, iokit/Kernel/RedwoodWLAN.cpp). That service runs
 * DHCP by itself, and it has to be the one that does: wifid takes the interface for usable only
 * when the address appears under a service whose settings name en0, and the service "dhcp en0"
 * makes has none (Settings then shows a spinner where the tick belongs). Only a kernel without
 * those IOKit objects gets the call.
 */
static void
wifi(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	double start = seconds();
	bool wanted = false, there = false;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *word = strtok(arguments, " "); word != NULL; word = strtok(NULL, " ")) {
		wanted |= strcmp(word, "wifi=1") == 0;
	}
	if (!wanted) {
		return;
	}
	while (!(there = if_nametoindex("en0") != 0) && seconds() - start < 120) {
		usleep(500000);
	}
	if (!there) {
		printf("net_test: wifi: no en0 after %.0f s: the radio has not come up\n", seconds() - start);
		return;
	}
	printf("net_test: wifi: en0 is there after %.1f s\n", seconds() - start);
	for (;;) {
		SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test"), NULL, NULL);
		NSArray *keys = session ? CFBridgingRelease(SCDynamicStoreCopyKeyList(session, CFSTR("Setup:/Network/Service/[^/]+/Interface"))) : nil;
		bool configd_has_it = false;

		for (NSString *key in keys) {
			NSDictionary *interface = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));

			configd_has_it |= [interface isKindOfClass:NSDictionary.class] && [interface[@"DeviceName"] isEqual:@"en0"];
		}
		if (session != NULL) {
			CFRelease(session);
		}
		if (configd_has_it) {
			printf("net_test: wifi: configd has a network service for en0 after %.1f s: it configures it itself\n", seconds() - start);
			return;
		}
		if (seconds() - start > 30) {
			break;
		}
		usleep(500000);
	}
	printf("net_test: wifi: configd has no network service for en0 (a kernel without its IOKit objects): DHCP asked for\n");
	dhcp("en0");
}

/* ---- watch ------------------------------------------------------------------------------------------ */

static double watch_start;

static void
watch_changed(SCDynamicStoreRef session, CFArrayRef keys, void *info)
{
	for (CFIndex i = 0; i < CFArrayGetCount(keys); i++) {
		NSString *key = (__bridge NSString *)CFArrayGetValueAtIndex(keys, i);
		id value = CFBridgingRelease(SCDynamicStoreCopyValue(session, (__bridge CFStringRef)key));
		NSString *names = [value isKindOfClass:NSDictionary.class] ?
		    [[[(NSDictionary *)value allKeys] sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@" "] : @"";

		printf("net_test: watch +%.2f s: %s %s%s%s\n", seconds() - watch_start, key.UTF8String, value ? "set" : "REMOVED",
		    value ? ": " : "", names.UTF8String);
	}
}

static void
watch(double limit)
{
	SCDynamicStoreRef session = SCDynamicStoreCreate(NULL, CFSTR("net_test watch"), (void *)watch_changed, NULL);
	NSArray *patterns = @[ @"State:/Network/Service/[^/]+/IPv4", @"State:/Network/Interface/en0/.*", @"State:/Network/Global/IPv4" ];
	CFRunLoopSourceRef source;

	if (session == NULL || !SCDynamicStoreSetNotificationKeys(session, NULL, (__bridge CFArrayRef)patterns) ||
	    (source = SCDynamicStoreCreateRunLoopSource(NULL, session, 0)) == NULL) {
		printf("net_test: watch: no session with configd\n");
		return;
	}
	watch_start = seconds();
	printf("net_test: watch: for %.0f s\n", limit);
	CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, limit, false);
	CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source, kCFRunLoopDefaultMode);
	CFRelease(source);
	CFRelease(session);
}

/* ---- wrongjoin --------------------------------------------------------------------------------------- */

struct apple80211req {
	char name[16];
	int32_t type, value;
	uint32_t length;
	void *data __attribute__((aligned(8)));
};

#define SIOCSA80211     0x802869c8u
#define SIOCGA80211     0xc02869c9u

static void
wrongjoin(void)
{
	static const char wrong[] = "not-the-passphrase-0";     /* (a passphrase nobody has: the test is that it fails) */
	uint8_t ssid[32], assoc[592];
	struct apple80211req request = { .name = "en0" };
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	double start;
	uint32_t length;

	request.type = 1;
	request.length = sizeof(ssid);
	request.data = ssid;
	if (s < 0 || ioctl(s, SIOCGA80211, &request) != 0 || request.length == 0 || request.length > sizeof(ssid)) {
		printf("net_test: wrongjoin: no network is joined (or no radio): nothing to try\n");
		if (s >= 0) {
			close(s);
		}
		return;
	}
	length = request.length;
	/* the layout of iOS 16.5 (bsd/dev/redwood_apple80211.c of the kernel): version, mode, authentication lower and
	 * upper, the name's length and the name, the access point (none: any), the key's version, length and kind (10:
	 * a passphrase as typed), the key */
	memset(assoc, 0, sizeof(assoc));
	*(uint32_t *)(assoc + 0x00) = 1;
	*(uint16_t *)(assoc + 0x04) = 1;
	*(uint16_t *)(assoc + 0x06) = 1;
	*(uint16_t *)(assoc + 0x08) = 8;
	*(uint32_t *)(assoc + 0x0c) = length;
	memcpy(assoc + 0x10, ssid, length);
	*(uint32_t *)(assoc + 0x38) = 1;
	*(uint32_t *)(assoc + 0x3c) = sizeof(wrong) - 1;
	*(uint32_t *)(assoc + 0x40) = 10;
	memcpy(assoc + 0x48, wrong, sizeof(wrong) - 1);
	memset(&request, 0, sizeof(request));
	strlcpy(request.name, "en0", sizeof(request.name));
	request.type = 20;
	request.length = sizeof(assoc);
	request.data = assoc;
	start = seconds();
	if (ioctl(s, SIOCSA80211, &request) != 0) {
		printf("net_test: wrongjoin: the kernel refuses the request: %s\n", strerror(errno));
		close(s);
		return;
	}
	printf("net_test: wrongjoin: a join with a wrong passphrase is asked for (a name of %u bytes)\n", length);
	for (;;) {
		memset(&request, 0, sizeof(request));
		strlcpy(request.name, "en0", sizeof(request.name));
		request.type = 21;
		if (ioctl(s, SIOCGA80211, &request) != 0) {
			printf("net_test: wrongjoin: the result cannot be asked for: %s\n", strerror(errno));
			break;
		}
		if (request.value != 0 || seconds() - start > 60) {
			printf("net_test: wrongjoin: the result is %d after %.1f s%s\n", request.value, seconds() - start, request.value == 21 ?
			    " (Apple80211's \"invalid PMK\": a wrong passphrase)" : request.value == 0 ? " (NOT OVER)" : "");
			break;
		}
		usleep(200000);
	}
	close(s);
}

/* ---- the panel -------------------------------------------------------------------------------------- */

/*
 * fbwait SECONDS: the first swap on this kernel comes at about 80 s (SpringBoard's first frame; the
 * kernel's framebuffer has no picture to give before it, and fbgrab fails with EINVAL). Rather than
 * guess the moment in a job list, wait for it here. The sysctl's handler copies the panel before the
 * kernel notices the 16-byte buffer is too small, so ask once a second, not once a millisecond.
 */
static void
fbwait(double limit)
{
	uint8_t header[16] = { 0 };
	double start = seconds();

	for (;;) {
		size_t size = sizeof(header);
		int error = sysctlbyname("kern.redwood.fbgrab", header, &size, NULL, 0);

		if (error != 0 && errno == EINVAL) {
			if (seconds() - start >= limit) {
				printf("net_test: fbwait: no frame after %.1f s\n", seconds() - start);
				return;
			}
			usleep(1000000);
			continue;
		}
		if (size >= 8) {
			printf("net_test: fbwait: a frame after %.1f s (%u x %u)\n", seconds() - start, ((uint32_t *)header)[0],
			    ((uint32_t *)header)[1]);
		} else {
			printf("net_test: fbwait: a frame after %.1f s\n", seconds() - start);
		}
		return;
	}
}

/*
 * end: the run stops here, not at loader_return_after a few seconds later. The kernel's console keeps
 * the last ~261 KB of a run's log and the device's own noise (the geod lookups of a widget: 84 KB in
 * 0.7 s, run 83) fills that within a second or two -- a picture of 25 KB written 16 s before the end
 * was gone by the time the log was read. Said through kern.redwood.end: the GPU is given back in order
 * and control returns to the loader (bsd/dev/redwood_mali.c).
 */
static void
end_run(void)
{
	int asked = 1;

	printf("net_test: end: asking the kernel to end the run here\n");
	fflush(stdout);
	if (sysctlbyname("kern.redwood.end", NULL, NULL, &asked, sizeof(asked)) != 0) {
		printf("net_test: end: %s\n", strerror(errno));
	}
	/* (if the kernel did not take it: loader_return_after still ends the run) */
	sleep(30);
}

/* ---- radio ------------------------------------------------------------------------------------------ */

/*
 * radio on|off: the radio's power, as iOS's Wi-Fi side asks for it (a SET of type 19, POWER: a
 * version, the number of radios and each one's state). Off leaves the network and stops the station
 * (bsd/dev/redwood_mtkwifi.c, wf_op_power); on makes the station alive again, and the network the
 * kernel was given in a file is joined again by itself (bsd/dev/redwood_sta.c) -- what iOS does with
 * the networks it knows. It is what Settings' Wi-Fi switch does through wifid, and what the run this
 * is written for checks: the link, the address and traffic, off and on again.
 */
static void
radio(const char *state)
{
	struct {
		uint32_t version, radios, state[4];
	} power = { 1, 1, { 0 } };
	struct apple80211req request;
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	if (strcmp(state, "on") != 0 && strcmp(state, "off") != 0) {
		printf("net_test: radio: on or off, not \"%s\"\n", state);
		return;
	}
	power.state[0] = strcmp(state, "on") == 0;
	memset(&request, 0, sizeof(request));
	strlcpy(request.name, "en0", sizeof(request.name));
	request.type = 19;
	request.length = sizeof(power);
	request.data = &power;
	if (s < 0) {
		printf("net_test: radio %s: no socket: %s\n", state, strerror(errno));
		return;
	}
	if (ioctl(s, SIOCSA80211, &request) != 0) {
		printf("net_test: radio %s: the kernel refuses: %s\n", state, strerror(errno));
		close(s);
		return;
	}
	close(s);
	printf("net_test: radio %s: asked\n", state);
	usleep(500000);
	show("en0", false);
}

/* ---- sbicons --------------------------------------------------------------------------------------- */

/*
 * The home screen's icon state, as SpringBoard keeps it: every bundle id with its place in the
 * nested pages/rows. A run cannot see the screen, so this is how the place of an installed app is
 * found without guessing: the index path says which page, which row and which column it is in.
 */
static void
sb_walk(CFTypeRef node, const char *path)
{
	char sub[320], text[256];

	if (node == NULL) {
		return;
	}
	if (CFGetTypeID(node) == CFStringGetTypeID()) {
		if (CFStringGetCString((CFStringRef)node, text, sizeof(text), kCFStringEncodingUTF8)) {
			printf("net_test: sbicons %s = %s\n", path, text);
		}
	} else if (CFGetTypeID(node) == CFArrayGetTypeID()) {
		CFArrayRef array = (CFArrayRef)node;

		for (CFIndex k = 0; k < CFArrayGetCount(array); k++) {
			snprintf(sub, sizeof(sub), "%s[%ld]", path, (long)k);
			sb_walk(CFArrayGetValueAtIndex(array, k), sub);
		}
	} else if (CFGetTypeID(node) == CFDictionaryGetTypeID()) {
		CFDictionaryRef dict = (CFDictionaryRef)node;
		const void *keys[16], *values[16];
		CFIndex count = CFDictionaryGetCount(dict);

		if (count > 16) {
			count = 16;
		}
		CFDictionaryGetKeysAndValues(dict, keys, values);
		for (CFIndex k = 0; k < count; k++) {
			if (CFGetTypeID(keys[k]) == CFStringGetTypeID() &&
			    CFStringGetCString((CFStringRef)keys[k], text, sizeof(text), kCFStringEncodingUTF8)) {
				snprintf(sub, sizeof(sub), "%s.%s", path, text);
				sb_walk(values[k], sub);
			}
		}
	}
}

static void
sbicons(void)
{
	const char *path = "/private/var/mobile/Library/Preferences/com.apple.springboard.plist";
	FILE *file = fopen(path, "rb");
	long size;
	UInt8 *bytes;
	CFDataRef data;
	CFPropertyListRef plist;
	CFTypeRef icon;

	if (file == NULL) {
		printf("net_test: sbicons: %s: %s\n", path, strerror(errno));
		return;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	bytes = malloc(size > 0 ? (size_t)size : 1);
	if (bytes == NULL || fread(bytes, 1, (size_t)size, file) != (size_t)size) {
		printf("net_test: sbicons: cannot read %s\n", path);
		free(bytes);
		fclose(file);
		return;
	}
	fclose(file);
	data = CFDataCreateWithBytesNoCopy(NULL, bytes, (CFIndex)size, kCFAllocatorNull);
	plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	free(bytes);
	if (plist == NULL) {
		printf("net_test: sbicons: the plist does not parse\n");
		return;
	}
	icon = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("iconState"));
	if (icon != NULL) {
		sb_walk(icon, "iconState");
	} else {
		/* No iconState key: say what keys there are, and walk the whole plist so the app's
		 * place is found whatever the key is called (SpringBoard may not have persisted it yet). */
		const void *keys[32];
		CFIndex count = CFDictionaryGetCount((CFDictionaryRef)plist);
		char key[128];

		if (count > 32) {
			count = 32;
		}
		CFDictionaryGetKeysAndValues((CFDictionaryRef)plist, keys, NULL);
		printf("net_test: sbicons: no iconState; %ld top-level keys:\n", (long)CFDictionaryGetCount((CFDictionaryRef)plist));
		for (CFIndex k = 0; k < count; k++) {
			if (CFGetTypeID(keys[k]) == CFStringGetTypeID() &&
			    CFStringGetCString((CFStringRef)keys[k], key, sizeof(key), kCFStringEncodingUTF8)) {
				printf("net_test: sbicons: key %s\n", key);
			}
		}
		sb_walk(plist, "root");
	}
	CFRelease(plist);
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	@autoreleasepool {
		for (int i = 1; i < argc; i++) {
			const char *step = argv[i];

			if (strcmp(step, "show") == 0) {
				show(NULL, false);
			} else if (strcmp(step, "up") == 0 && i + 1 < argc) {
				up(argv[++i]);
			} else if (strcmp(step, "lookup") == 0 && i + 1 < argc) {
				lookup(argv[++i]);
			} else if (strcmp(step, "procs") == 0 && i + 1 < argc) {
				procs(argv[++i]);
			} else if (strcmp(step, "dir") == 0 && i + 1 < argc) {
				directory(argv[++i]);
			} else if (strcmp(step, "radio") == 0 && i + 1 < argc) {
				radio(argv[++i]);
			} else if (strcmp(step, "fbwait") == 0 && i + 1 < argc) {
				fbwait(atof(argv[++i]));
			} else if (strcmp(step, "end") == 0) {
				end_run();
			} else if (strcmp(step, "stop") == 0 && i + 1 < argc) {
				signal_named(argv[++i], SIGSTOP, "stop");
			} else if (strcmp(step, "cont") == 0 && i + 1 < argc) {
				signal_named(argv[++i], SIGCONT, "cont");
			} else if (strcmp(step, "watchdie") == 0 && i + 1 < argc) {
				const char *word = argv[++i];
				double limit = i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9' ? atof(argv[++i]) : 30;

				watchdie(word, limit);
			} else if (strcmp(step, "me") == 0) {
				printf("net_test: me: pid %d, uid %d, euid %d, gid %d\n", (int)getpid(), (int)getuid(), (int)geteuid(),
				    (int)getgid());
			} else if (strcmp(step, "store") == 0) {
				store(i + 1 < argc && strchr(argv[i + 1], ':') != NULL ? argv[++i] : NULL);
			} else if (strcmp(step, "dhcp") == 0 && i + 1 < argc) {
				dhcp(argv[++i]);
			} else if (strcmp(step, "waitaddr") == 0 && i + 2 < argc) {
				const char *interface = argv[++i];
				double start = seconds(), limit = atof(argv[++i]);

				while (!show(interface, true) && seconds() - start < limit) {
					usleep(250000);
				}
				printf("net_test: waitaddr %s: %s after %.1f s\n", interface, show(interface, true) ? "an address" :
				    "NO address", seconds() - start);
				show(interface, false);
			} else if (strcmp(step, "resolve") == 0 && i + 1 < argc) {
				resolve(argv[++i], NULL, 0);
			} else if (strcmp(step, "tcp") == 0 && i + 2 < argc) {
				tcp(argv[i + 1], argv[i + 2]);
				i += 2;
			} else if (strcmp(step, "fetch") == 0 && i + 1 < argc) {
				const char *address = argv[++i];
				double limit = i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9' ? atof(argv[++i]) : 60;

				fetch(address, limit);
			} else if (strcmp(step, "reach") == 0 && i + 1 < argc) {
				reach(argv[++i]);
			} else if (strcmp(step, "wait") == 0 && i + 1 < argc) {
				usleep((useconds_t)(atof(argv[++i]) * 1e6));
			} else if (strcmp(step, "name") == 0) {
				device_name();
			} else if (strcmp(step, "wifi") == 0) {
				/* the job com.redwood.wifi is the one of ours that every image with configd runs at
				 * its start: the name goes with it, beside the wait for the radio and not before it */
				fflush(stdout);
				if (fork() == 0) {
					device_name();
					fflush(stdout);
					_exit(0);
				}
				wifi();
			} else if (strcmp(step, "watch") == 0 && i + 1 < argc) {
				watch(atof(argv[++i]));
			} else if (strcmp(step, "wrongjoin") == 0) {
				wrongjoin();
			} else if (strcmp(step, "waitproc") == 0 && i + 1 < argc) {
				const char *word = argv[++i];
				double limit = i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9' ? atof(argv[++i]) : 60;

				waitproc(word, limit);
			} else if (strcmp(step, "sbicons") == 0) {
				sbicons();
			} else {
				fprintf(stderr, "net_test: bad step \"%s\"\n", step);
				return 1;
			}
		}
	}
	return 0;
}
