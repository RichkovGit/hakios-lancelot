/*
 * libredwood-nil.dylib: inside Settings, the dictionary whose nil object kills it (see inject.c for how
 * it gets there; build.sh's INJECT names the programs, NILINJECT=Preferences).
 *
 * Run 88 (logs/lancelot-connsys38.log) ended at the program's own words, with the boot args
 * applog=1 applog_name=Preferences:
 *
 *   *** Terminating app due to uncaught exception 'NSInvalidArgumentException', reason:
 *   '*** -[__NSPlaceholderDictionary initWithObjects:forKeys:count:]: attempt to insert nil object
 *   from objects[3]'
 *
 * about one second after it opened, in -[PreferenceSceneDelegate scene:willConnectToSession:options:].
 * Run 91 (logs/lancelot-connsys41.log) had every daemon by then -- accountsd.accountmanager, nehelper,
 * corefollowup.agent all "there" -- and it died at 1.09 s all the same (spawned 22:47:22.446, SIGABRT
 * 22:47:23.534): the missing neighbours were not the reason.
 *
 * A class cluster's placeholder is the one place every NSDictionary made from arrays goes through, so
 * this replaces -[__NSPlaceholderDictionary initWithObjects:forKeys:count:] with a version that, when
 * an object is nil, says which index, which classes and which keys, and the frames of the stack, and
 * then builds the dictionary without that pair: the program goes on instead of aborting, and what it
 * does with the hole says the rest.
 *
 * The lines go to kern.redwood.say with "!" in front (kern.redwood.say, redwood_mali.c): the kernel
 * keeps those and says them again every minute, and puts them in the buffer the host gets, so they
 * survive the widget noise that eats the console's last two seconds.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/sysctl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MOST_KEYS       32      /* a dictionary with more keys than this is not one of those */
#define MOST_REPORTS    8

static IMP original_init;
static int reports;

/* a line the kernel keeps (and says again every minute) */
static void
kept(const char *format, ...)
{
	char line[240], sent[248];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	snprintf(sent, sizeof(sent), "!%s", line);
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, sent, strlen(sent));
	/*
	 * ... and on the standard error, which the applog boot argument points at the console: an
	 * application is sandboxed, and its write to kern.redwood.say is refused, while the console it
	 * was given at exec is its own.
	 */
	(void)write(2, sent, strlen(sent));
	(void)write(2, "\n", 1);
}

/* the text of a key that is a string, else its class; never calls anything that could build a dictionary */
static const char *
name_of(id object, char *buffer, size_t size)
{
	Class cls = object != NULL ? object_getClass(object) : Nil;
	const char *clsname = cls != Nil ? class_getName(cls) : "nil";

	if (object != NULL && strstr(clsname, "String") != NULL) {
		const char *text = ((const char *(*)(id, SEL))objc_msgSend)(object, sel_registerName("UTF8String"));

		if (text != NULL && text[0] != 0) {
			snprintf(buffer, size, "%.40s", text);
			return buffer;
		}
	}
	snprintf(buffer, size, "(%s)", clsname);
	return buffer;
}

/*
 * The frames of a stack, one line each, as image + offset (dladdr, which is what names a framework
 * that is in the dyld shared cache as well as an application's own binary), and the symbol when the
 * image carries one: "PreferencesUI+1a2c ...". A line each, because a line is what the console takes.
 */
static void
say_frames(const char *what, void *const *frames, int n)
{
	for (int i = 0; i < n; i++) {
		Dl_info info;

		if (dladdr(frames[i], &info) && info.dli_fname != NULL && info.dli_fbase != NULL) {
			const char *base = strrchr(info.dli_fname, '/');

			kept("%s %d %s+%lx%s%s", what, i, base != NULL ? base + 1 : info.dli_fname,
			    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)info.dli_fbase),
			    info.dli_sname != NULL ? " " : "", info.dli_sname != NULL ? info.dli_sname : "");
		} else {
			kept("%s %d %lx (no image)", what, i, (unsigned long)(uintptr_t)frames[i]);
		}
	}
}

/*
 * What kills it without a word: a signal. The exception at 1.09 s said its reason on the standard
 * error, but the death at 8.8 s of run 94 said nothing at all in the console -- and the kernel only
 * said "Corpse allowed 1 of 5". So the signals are caught, said with their frames, and passed on.
 */
static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	int n = backtrace(frames, 32);

	(void)context;
	kept("nilfix: killed by signal %d, code %d, address %p", number, info != NULL ? info->si_code : 0,
	    info != NULL ? info->si_addr : NULL);
	say_frames("nilfix: signal frame", frames, n);
	signal(number, SIG_DFL);
	raise(number);
}

static id
nil_init(id self, SEL _cmd, const id *objects, const id *keys, unsigned long count)
{
	id (*original)(id, SEL, const id *, const id *, unsigned long) = (void *)original_init;
	unsigned long bad = count;

	if (objects == NULL || keys == NULL || count == 0 || count > MOST_KEYS) {
		return original(self, _cmd, objects, keys, count);
	}
	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] == Nil || keys[i] == Nil) {
			bad = i;
			break;
		}
	}
	if (bad == count) {
		return original(self, _cmd, objects, keys, count);
	}
	if (reports++ < MOST_REPORTS) {
		char text[64];
		void *frames[24];
		int n = backtrace(frames, 24);

		kept("nilfix: a dictionary of %lu keys, object %lu is nil: %s", (unsigned long)count, (unsigned long)bad,
		    name_of(keys[bad], text, sizeof(text)));
		for (unsigned long i = 0; i < count; i++) {
			char key[64];

			kept("nilfix: key %lu %s -> %s", (unsigned long)i, name_of(keys[i], key, sizeof(key)),
			    objects[i] != Nil ? class_getName(object_getClass(objects[i])) : "nil");
		}
		say_frames("nilfix: frame", frames, n);
	}
	{
		id better_objects[MOST_KEYS], better_keys[MOST_KEYS];
		unsigned long better = 0;

		for (unsigned long i = 0; i < count; i++) {
			if (objects[i] != Nil && keys[i] != Nil) {
				better_objects[better] = objects[i];
				better_keys[better] = keys[i];
				better++;
			}
		}
		return original(self, _cmd, better_objects, better_keys, better);
	}
}

__attribute__((constructor))
static void
install_once(void)
{
	Class placeholder = objc_getClass("__NSPlaceholderDictionary");
	Method method = placeholder != Nil ? class_getInstanceMethod(placeholder, sel_registerName("initWithObjects:forKeys:count:")) : NULL;

	if (method != NULL) {
		original_init = method_setImplementation(method, (IMP)nil_init);
	}
	{
		static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
		struct sigaction action = { 0 };

		action.sa_sigaction = killed;
		action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
		sigemptyset(&action.sa_mask);
		for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
			sigaction(signals[i], &action, NULL);
		}
	}
	kept("nilfix: %s, hook %s", getprogname(), method != NULL ? "installed" : "NOT installed (no such class or method)");
}