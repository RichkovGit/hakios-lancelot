/* lsapp: is the A2 app registered with LaunchServices, and can it be registered?
 * SBSLaunchApplicationWithIdentifier returned 9 (runs 77-82) with and without entitlements,
 * which points at LaunchServices registration rather than authorization. This probes the
 * LaunchServices framework directly and writes the answer to host_keep so it survives the run. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <string.h>
#include <sys/sysctl.h>

static void
keep(const char *format, ...)
{
	char line[256];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	printf("lsapp: %s\n", line);
	sysctlbyname("kern.redwood.host_keep", NULL, NULL, line, strlen(line));
}

int
main(void)
{
	@autoreleasepool {
		/* LaunchServices is already loaded (Foundation/CoreServices pull it in), so its symbols
		 * are in the global space; dlopen by its on-disk path failed in run 83 (cache-only). */
		void *ls = RTLD_DEFAULT;
		CFStringRef bid = CFSTR("com.redwood.bt_test");
		CFURLRef url = CFURLCreateWithFileSystemPath(NULL, CFSTR("/Applications/bt_test.app"), kCFURLPOSIXPathStyle, true);
		CFTypeRef (*copyForBundle)(CFStringRef) = NULL;
		OSStatus (*regURL)(CFURLRef, Boolean) = NULL;
		int (*regURL2)(CFURLRef, CFDictionaryRef, unsigned int, CFErrorRef *) = NULL;
		CFTypeRef app;

		copyForBundle = (CFTypeRef (*)(CFStringRef))dlsym(ls, "LSApplicationCopyForBundleID");
		if (copyForBundle == NULL) {
			copyForBundle = (CFTypeRef (*)(CFStringRef))dlsym(ls, "_LSCopyApplicationForBundleID");
		}
		if (copyForBundle == NULL) {
			void *h = dlopen("/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/LaunchServices", RTLD_NOW);
			keep("dlopen LaunchServices fallback = %p", h);
			if (h != NULL) {
				ls = h;
				copyForBundle = (CFTypeRef (*)(CFStringRef))dlsym(ls, "LSApplicationCopyForBundleID");
			}
		}
		keep("LSApplicationCopyForBundleID symbol = %p", (void *)copyForBundle);
		if (copyForBundle != NULL) {
			app = copyForBundle(bid);
			keep("before: registered = %d (app %p)", app != NULL, (void *)app);
			if (app != NULL) {
				CFRelease(app);
			}
		}
		regURL = (OSStatus (*)(CFURLRef, Boolean))dlsym(ls, "LSRegisterURL");
		keep("LSRegisterURL symbol = %p", (void *)regURL);
		if (regURL != NULL) {
			OSStatus s = regURL(url, true);
			keep("LSRegisterURL(/Applications/bt_test.app, update) = %d", (int)s);
		}
		regURL2 = (int (*)(CFURLRef, CFDictionaryRef, unsigned int, CFErrorRef *))dlsym(ls, "_LSApplicationRegisterURL");
		keep("_LSApplicationRegisterURL symbol = %p", (void *)regURL2);
		if (regURL2 != NULL) {
			CFErrorRef err = NULL;
			int r = regURL2(url, NULL, 0, &err);
			keep("_LSApplicationRegisterURL = %d err %p", r, (void *)err);
			if (err != NULL) {
				CFStringRef desc = CFErrorCopyDescription(err);
				char buf[200];
				if (desc != NULL && CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8)) {
					keep("register error: %s", buf);
				}
				if (desc != NULL) {
					CFRelease(desc);
				}
				CFRelease(err);
			}
		}
		if (copyForBundle != NULL) {
			app = copyForBundle(bid);
			keep("after: registered = %d (app %p)", app != NULL, (void *)app);
			if (app != NULL) {
				CFRelease(app);
			}
		}
		CFRelease(url);
	}
	return 0;
}
