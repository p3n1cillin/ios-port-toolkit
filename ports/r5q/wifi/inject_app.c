/*
 * libr5q-app.dylib (Test 146; was libr5q-crash.dylib, Test 145): inside the stock applications.
 *
 * 1. No OpenGL ES where there is none. This image has no GPU driver and no OpenGL ES of ours (GL=1, eagl.c), yet the
 *    stock EAGLContext still answers -initWithAPI: with a context whose GL dispatch table is all zeros; CoreImage, with
 *    no Metal device, takes that context and calls glDisable(GL_DEPTH_TEST) through it -- a jump to 0 (Test 145:
 *    Messages, ChatKit -> MobileIcons LICreateIconForImages -> CoreImage +0x19c588, w1 = 0xb71). Here -initWithAPI:
 *    and -initWithAPI:sharegroup: answer nil instead, as on a device without OpenGL ES, and CoreImage draws by itself.
 *
 * 2. Where a stock program dies (Test 145). Injected (etc/inject.conf, the kernel's exec_inject) into
 * a program that crashes at start (Messages: SIGSEGV 0.25 s in, right after its CoreTelephony lookups fail); on
 * SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGTRAP it says on the console the faulting address, the registers that matter, and
 * the stack by frame pointers with each address's image and nearest symbol (dladdr), plus the ObjC selector in x1 when
 * the fault is in objc_msgSend; then lets the signal take the program as before. Nothing else is changed.
 * Built with the iPhoneOS 16.5 SDK.
 */
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <mach-o/dyld.h>

static int console = -1;

static void
say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > 0) {
		(void)write(console >= 0 ? console : 2, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
}

static void
where(const char *what, uintptr_t address)
{
	Dl_info info;

	if (address != 0 && dladdr((const void *)address, &info) != 0) {
		const char *image = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;

		say("r5q-crash: %s %#lx %s+%#lx (%s+%#lx)\n", what, (unsigned long)address, info.dli_sname ? info.dli_sname : "?",
		    info.dli_saddr ? (unsigned long)(address - (uintptr_t)info.dli_saddr) : 0UL,
		    image ? image + 1 : (info.dli_fname ? info.dli_fname : "?"),
		    (unsigned long)(address - (uintptr_t)info.dli_fbase));
	} else {
		say("r5q-crash: %s %#lx (no image)\n", what, (unsigned long)address);
	}
}

static void
crashed(int signal, siginfo_t *info, void *context)
{
	ucontext_t *uc = context;
	char name[64] = "?";
	uintptr_t pc = 0, lr = 0, fp = 0, sp = 0, x0 = 0, x1 = 0;

	if (uc != NULL && uc->uc_mcontext != NULL) {
		pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		lr = (uintptr_t)uc->uc_mcontext->__ss.__lr;
		fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
		sp = (uintptr_t)uc->uc_mcontext->__ss.__sp;
		x0 = (uintptr_t)uc->uc_mcontext->__ss.__x[0];
		x1 = (uintptr_t)uc->uc_mcontext->__ss.__x[1];
	}
	{
		const char *p = getprogname();

		if (p != NULL) {
			strlcpy(name, p, sizeof(name));
		}
	}
	say("r5q-crash: %s[%d] signal %d, code %d, fault address %#lx; pc %#lx lr %#lx sp %#lx fp %#lx x0 %#lx x1 %#lx\n", name,
	    getpid(), signal, info ? info->si_code : 0, info ? (unsigned long)info->si_addr : 0UL, (unsigned long)pc,
	    (unsigned long)lr, (unsigned long)sp, (unsigned long)fp, (unsigned long)x0, (unsigned long)x1);
	where("pc", pc);
	where("lr", lr);
	{
		Dl_info dl;

		if (pc != 0 && dladdr((const void *)pc, &dl) != 0 && dl.dli_sname != NULL && strstr(dl.dli_sname, "objc_msgSend") != NULL &&
		    x1 != 0) {
			say("r5q-crash: the selector (x1): %.80s\n", (const char *)x1);
		}
	}
	for (int depth = 0; depth < 40 && fp != 0 && (fp & 7) == 0; depth++) {
		uintptr_t *frame = (uintptr_t *)fp;
		char what[16];

		snprintf(what, sizeof(what), "#%d", depth);
		where(what, frame[1] & 0x0000000fffffffffUL);
		if (frame[0] <= fp) {
			break;
		}
		fp = frame[0];
	}
	{
		struct sigaction dfl;

		memset(&dfl, 0, sizeof(dfl));
		dfl.sa_handler = SIG_DFL;
		sigaction(signal, &dfl, NULL);
	}
	raise(signal);
}

/*
 * (Test 146) MobileIcons' icon maker: CoreImage, with neither Metal nor OpenGL ES, still takes a GL path whose dispatch
 * table is empty (EAGLContext is not the way in: answering nil there changed nothing). Where there is no OpenGL ES
 * the icon is the image itself, unmasked: a square icon instead of a program that dies. LICreateIconForImage(image,
 * variant, options) puts the image in an array and calls LICreateIconForImages(array, variant, options).
 */
typedef const void *CFTypeRef_;
extern const void *LICreateIconForImages(const void *images, int variant, int options);
extern long CFArrayGetCount(const void *array);
extern const void *CFArrayGetValueAtIndex(const void *array, long index);
extern const void *CGImageRetain(const void *image);
static int gl_absent = -1;
static unsigned icons_made;

static const void *
icon_without_gl(const void *images, int variant, int options)
{
	if (gl_absent == -1) {
		gl_absent = 1;          /* (Test 167: our OpenGL ES is the window server's, not the apps') */
	}
	if (!gl_absent) {
		return LICreateIconForImages(images, variant, options);
	}
	if (++icons_made <= 3) {
		say("r5q-app: %s: an icon (variant %d, options %#x) made without CoreImage: the image itself\n", getprogname(), variant,
		    options);
	}
	if (images != NULL && CFArrayGetCount(images) > 0) {
		const void *image = CFArrayGetValueAtIndex(images, 0);

		return image != NULL ? CGImageRetain(image) : NULL;
	}
	return NULL;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *original;
} interposed[] = {
	{ (const void *)icon_without_gl, (const void *)LICreateIconForImages },
};

/*
 * (Test 147) CoreImage on the processors. With no Metal device and no OpenGL ES, a CIContext made without options
 * takes CoreImage's GL path anyway and draws through an all-zero dispatch table (Messages' icons: a jump to 0). Every
 * -[CIContext initWithOptions:] is given kCIContextUseSoftwareRenderer = YES here, CoreImage's documented way to its
 * software renderer.
 */
static id (*ci_init_original)(id, SEL, id);
static unsigned ci_contexts;

static id
ci_init_software(id self, SEL selector, id options)
{
	id key = NULL, dict;
	void *symbol = dlsym(RTLD_DEFAULT, "kCIContextUseSoftwareRenderer");
	Class mutable = objc_getClass("NSMutableDictionary");

	if (symbol != NULL) {
		key = *(id *)symbol;
	}
	if (key == NULL || mutable == NULL) {
		return ci_init_original(self, selector, options);
	}
	dict = options != NULL ? ((id (*)(Class, SEL, id))objc_msgSend)(mutable, sel_registerName("dictionaryWithDictionary:"), options) :
	    ((id (*)(Class, SEL))objc_msgSend)(mutable, sel_registerName("dictionary"));
	((void (*)(id, SEL, id, id))objc_msgSend)(dict, sel_registerName("setObject:forKey:"),
	    (id)(*(void **)dlsym(RTLD_DEFAULT, "kCFBooleanTrue")), key);
	if (++ci_contexts <= 3) {
		say("r5q-app: %s: a CIContext on the processors (software renderer)\n", getprogname());
	}
	return ci_init_original(self, selector, dict);
}

/* +[CIContext contextWithEAGLContext:(options:)] -> [[CIContext alloc] initWithOptions:options] (made software above) */
static id
ci_eagl_maker2(Class self, SEL selector, id eagl, id options)
{
	static unsigned told;

	/*
	 * (Test 147: a software context -- kCIContextUseSoftwareRenderer -- got Messages past the GL table, then it killed
	 * itself with SIGILL a second later: so no context at all, as where OpenGL ES fails outright.)
	 */
	(void)self;
	(void)selector;
	(void)eagl;
	(void)options;
	if (told++ < 3) {
		say("r5q-app: %s: +[CIContext contextWithEAGLContext:...] answers nil (no OpenGL ES)\n", getprogname());
	}
	return NULL;
}

static id
ci_eagl_maker1(Class self, SEL selector, id eagl)
{
	return ci_eagl_maker2(self, selector, eagl, NULL);
}

static void
coreimage_software(void)
{
	Class ci;
	Method m;
	id (*mtl)(void);

	/*
	 * (Test 167) Not skipped where /redwood/lib/libredwood-gl.dylib exists: that OpenGL ES is the window server's
	 * (backboardd) only. In an app EAGLContext is still the stock one with the empty dispatch table, and Messages
	 * crashed in CoreImage again with the GPU image (Test 166).
	 */
	mtl = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	if (mtl == NULL) {
		dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
		mtl = (id (*)(void))dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");
	}
	if (mtl != NULL && mtl() != NULL) {
		return;                 /* (a Metal device: CoreImage's own choice is right) */
	}
	dlopen("/System/Library/Frameworks/CoreImage.framework/CoreImage", RTLD_LAZY);
	ci = objc_getClass("CIContext");
	m = ci ? class_getInstanceMethod(ci, sel_registerName("initWithOptions:")) : NULL;
	if (m == NULL) {
		say("r5q-app: %s: no -[CIContext initWithOptions:]\n", getprogname());
		return;
	}
	(void)ci_init_software;         /* (Test 147b: initWithOptions: left as it is) */
	/*
	 * MobileIcons asks for a context on an EAGLContext (+[CIContext(Internal) internalContextWithEAGLContext:options:],
	 * CoreImage +0x4570c): with no OpenGL ES driver behind the EAGLContext, that is the all-zero dispatch table. The
	 * three class methods that make one answer a context on the processors instead (initWithOptions: above).
	 */
	{
		static const char *const makers[] = { "contextWithEAGLContext:", "contextWithEAGLContext:options:",
			                              "internalContextWithEAGLContext:options:" };
		Class meta = object_getClass((id)ci);

		for (unsigned i = 0; i < sizeof(makers) / sizeof(makers[0]); i++) {
			Method cm = class_getInstanceMethod(meta, sel_registerName(makers[i]));

			if (cm != NULL) {
				method_setImplementation(cm, i == 0 ? (IMP)ci_eagl_maker1 : (IMP)ci_eagl_maker2);
				say("r5q-app: %s: +[CIContext %s] makes a context on the processors\n", getprogname(), makers[i]);
			}
		}
	}
}

static id
no_context(id self, SEL selector, ...)
{
	(void)selector;
	if (self != NULL) {
		((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
	}
	return NULL;
}

static void
no_opengl_es(void)
{
	Class eagl;
	unsigned done = 0;

	if (access("/redwood/lib/libredwood-gl.dylib", F_OK) == 0) {
		return;                 /* (an OpenGL ES of ours is there: EAGLContext has something behind it) */
	}
	dlopen("/System/Library/Frameworks/OpenGLES.framework/OpenGLES", RTLD_LAZY);
	eagl = objc_getClass("EAGLContext");
	if (eagl == NULL) {
		say("r5q-app: %s: no EAGLContext class\n", getprogname());
		return;
	}
	{
		/* every initializer (Test 146: replacing -initWithAPI: and -initWithAPI:sharegroup: was not enough) */
		unsigned count = 0;
		Method *methods = class_copyMethodList(eagl, &count);

		for (unsigned i = 0; methods != NULL && i < count; i++) {
			const char *name = sel_getName(method_getName(methods[i]));

			if (strncmp(name, "initWith", 8) == 0) {
				method_setImplementation(methods[i], (IMP)no_context);
				say("r5q-app: -[EAGLContext %s] answers nil\n", name);
				done++;
			}
		}
		free(methods);
	}
	say("r5q-app: %s[%d]: no OpenGL ES here: EAGLContext answers nil (%u methods)\n", getprogname(), getpid(), done);
}

/*
 * (Test 174) In PosterBoard only: -[__NSPlaceholderDictionary initWithObjects:forKeys:count:] given a nil object writes
 * down the keys and leaves that pair out instead of throwing. PosterBoard died of exactly that ("attempt to insert nil
 * object from objects[1]", PosterBoard+0x71aa8) when the wallpapers were shown, and the lock screen stayed black.
 */
typedef id (*nd_init_fn)(id, SEL, const id *, const id *, unsigned long);
static nd_init_fn nd_original;

static void
nd_say(const char *text)
{
	write(2, text, strlen(text));
}

static const char *
nd_describe(id o, char *buf, size_t size)
{
	id d;
	const char *c;

	if (o == NULL) {
		return "(nil)";
	}
	d = ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName("description"));
	c = d ? ((const char *(*)(id, SEL))objc_msgSend)(d, sel_registerName("UTF8String")) : NULL;
	snprintf(buf, size, "%s", c ? c : "?");
	return buf;
}

static id
nd_checked_init(id self, SEL sel, const id *objects, const id *keys, unsigned long count)
{
	id o[64], k[64];
	unsigned long n = 0, nils = 0;
	char line[600], kb[200], ob[200];

	for (unsigned long i = 0; i < count; i++) {
		if (objects[i] == NULL || keys[i] == NULL) {
			nils++;
		}
	}
	if (nils == 0 || count > 64) {
		return nd_original(self, sel, objects, keys, count);
	}
	snprintf(line, sizeof(line), "r5q-nildict: %s[%d]: a dictionary of %lu with %lu nil:\n", getprogname(), getpid(), count, nils);
	nd_say(line);
	for (unsigned long i = 0; i < count; i++) {
		snprintf(line, sizeof(line), "r5q-nildict:   [%lu] %s = %s\n", i, nd_describe(keys[i], kb, sizeof(kb)),
		    nd_describe(objects[i], ob, sizeof(ob)));
		nd_say(line);
		if (objects[i] != NULL && keys[i] != NULL) {
			o[n] = objects[i];
			k[n] = keys[i];
			n++;
		}
	}
	return nd_original(self, sel, o, k, n);
}

static void
nil_dictionaries_survived(void)
{
	Class c = objc_getClass("__NSPlaceholderDictionary");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("initWithObjects:forKeys:count:")) : NULL;

	if (m != NULL) {
		nd_original = (nd_init_fn)method_setImplementation(m, (IMP)nd_checked_init);
		nd_say("r5q-nildict: watching dictionaries made with a nil object\n");
	}
}

__attribute__((constructor)) static void
start(void)
{
	static char stack[64 * 1024];
	stack_t alt = { .ss_sp = stack, .ss_size = sizeof(stack), .ss_flags = 0 };
	struct sigaction sa;
	const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGTRAP };

	console = open("/dev/console", O_WRONLY);
	sigaltstack(&alt, NULL);
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &sa, NULL);
	}
	/*
	 * (Test 146: no_opengl_es() is not called: answering nil from EAGLContext did not change Messages' crash --
	 * CoreImage's GL path does not go through EAGLContext -- and would change what other programs see. Neither did
	 * interposing LICreateIconForImages: inside the shared cache UIKit calls MobileIcons directly. Kept for reference.)
	 */
	(void)no_opengl_es;
	coreimage_software();
	if (strcmp(getprogname(), "PosterBoard") == 0) {
		nil_dictionaries_survived();
	}
}
