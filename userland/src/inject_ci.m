/*
 * libredwood-ci.dylib: a CIContext that works here, inside the programs that ask for the default one
 * (build.sh's CIINJECT names them, and puts the OpenGL ES library behind it in the same process).
 *
 * Run 96 (logs/lancelot-connsys48.log) is where this comes from. The dictionary with the nil icon
 * fixed (src/inject_nil.c), Settings opened and then died in 0.57 s with signal 11 at address 0:
 *
 *   nilfix: signal frame 2 CoreImage+19c58c
 *   nilfix: signal frame 4 CoreImage+45884    +[CIContext internalContextWithEAGLContext:options:] + 0x244
 *   nilfix: signal frame 5 CoreImage+7114     -[CIContext initWithOptions:] + 0x1f8
 *   nilfix: signal frame 6 MobileIcons+7ac0   (LICreateDefaultIcon's neighbourhood)
 *
 * That is the same death metal_test had already found and explained (docs/metal.md, section 7):
 * +[CIContext contextWithOptions:] goes straight to the OpenGL ES back end (CIGLIsUsable() is compiled
 * to "return 1"), CI::GLContext::init() runs with no EAGLContext at all, and dereferences nil. CoreImage
 * is not Metal-only: over an EAGLContext of ours -- and this port has one, src/eagl.c -- the context is
 * made and filters run ("CoreImage rendered: first pixel 0 229 255 255").
 *
 * So the default context is replaced with the one CoreImage can build here: the caller's options are
 * passed on, the EAGLContext is a fresh one per request (a GL context belongs to the thread that made
 * it current, and MobileIcons asks from a dispatch queue), and the GL library is loaded on the spot if
 * the program was not given it at exec -- the kernel's insertion is where /redwood/lib is reachable
 * before the sandbox is.
 *
 * The lines go to the standard error, which applog=... points at the console, and to kern.redwood.say:
 * an application's write there is refused (run 93/94: no "redwood: ... started" ever arrived), but
 * saying it on both costs nothing and a daemon's copy is kept.
 */
#import <Foundation/Foundation.h>
#import <CoreImage/CoreImage.h>
#import <OpenGLES/EAGL.h>
#include <objc/runtime.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define REDWOOD_GL      "/redwood/lib/libredwood-gl.dylib"

static IMP original_with_options;
static IMP original_context;
static int gl_state;                    /* 0 not tried, 1 loaded, -1 not there */
static int said_made, said_no_gl, said_failed;
static __thread int busy;               /* (a context made by us must not come back through the hook) */

static void
say(const char *format, ...)
{
	char line[220], sent[228];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	snprintf(sent, sizeof(sent), "!ci: %s", line);
	(void)sysctlbyname("kern.redwood.say", NULL, NULL, sent, strlen(sent));
	(void)write(2, sent, strlen(sent));
	(void)write(2, "\n", 1);
}

/* our OpenGL ES (src/eagl.c), if the program does not already have it */
static EAGLContext *
make_gl(void)
{
	EAGLContext *gl;

	if (gl_state == 0) {
		gl_state = dlopen(REDWOOD_GL, RTLD_NOW | RTLD_GLOBAL) != NULL ? 1 : -1;
		if (gl_state < 0) {
			say("%s: %s: %s", getprogname(), REDWOOD_GL, dlerror());
		}
	}
	gl = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
	if (gl == nil) {
		gl = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
	}
	if (gl == nil && !said_no_gl) {
		said_no_gl = 1;
		say("%s: no EAGLContext: this image has no OpenGL ES", getprogname());
	}
	return gl;
}

static CIContext *
context_over_gl(NSDictionary *options)
{
	EAGLContext *gl = make_gl();
	CIContext *context;

	if (gl == nil) {
		return nil;
	}
	/* CI::GLContext makes its own contexts on this one's sharegroup; it has to be current here */
	if (![EAGLContext setCurrentContext:gl]) {
		say("%s: the EAGLContext would not become current", getprogname());
	}
	context = [CIContext contextWithEAGLContext:gl options:options];
	if (context == nil) {
		if (!said_failed) {
			said_failed = 1;
			say("%s: contextWithEAGLContext: gave nil", getprogname());
		}
	} else if (!said_made) {
		said_made = 1;
		say("%s: a CIContext over our EAGLContext (API %d)", getprogname(), (int)[gl API]);
	}
	return context;
}

/* +[CIContext contextWithOptions:] */
static CIContext *
my_contextWithOptions(id self, SEL _cmd, NSDictionary *options)
{
	CIContext *context;

	if (busy || original_with_options == NULL) {
		return ((CIContext *(*)(id, SEL, NSDictionary *))original_with_options)(self, _cmd, options);
	}
	busy = 1;
	context = context_over_gl(options);
	busy = 0;
	return context;
}

/* +[CIContext context] */
static CIContext *
my_context(id self, SEL _cmd)
{
	CIContext *context;

	if (busy || original_context == NULL) {
		return ((CIContext *(*)(id, SEL))original_context)(self, _cmd);
	}
	busy = 1;
	context = context_over_gl(nil);
	busy = 0;
	return context;
}

__attribute__((constructor)) static void
install(void)
{
	Class ci = objc_getClass("CIContext");
	Method with_options = ci != NULL ? class_getClassMethod(ci, @selector(contextWithOptions:)) : NULL;
	Method plain = ci != NULL ? class_getClassMethod(ci, @selector(context)) : NULL;

	if (with_options != NULL) {
		original_with_options = method_setImplementation(with_options, (IMP)my_contextWithOptions);
	}
	if (plain != NULL) {
		original_context = method_setImplementation(plain, (IMP)my_context);
	}
	say("%s: hook %s", getprogname(),
	    with_options != NULL || plain != NULL ? "installed" : "NOT installed (no CIContext class)");
}