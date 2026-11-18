/*
 * libr5q-crash.dylib (Test 145): where a stock program dies. Injected (etc/inject.conf, the kernel's exec_inject) into
 * a program that crashes at start (Messages: SIGSEGV 0.25 s in, right after its CoreTelephony lookups fail); on
 * SIGSEGV/SIGBUS/SIGILL/SIGABRT/SIGTRAP it says on the console the faulting address, the registers that matter, and
 * the stack by frame pointers with each address's image and nearest symbol (dladdr), plus the ObjC selector in x1 when
 * the fault is in objc_msgSend; then lets the signal take the program as before. Nothing else is changed.
 * Built with the iPhoneOS 16.5 SDK.
 */
#include <dlfcn.h>
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
	say("r5q-crash: watching %s[%d]\n", getprogname(), getpid());
}
