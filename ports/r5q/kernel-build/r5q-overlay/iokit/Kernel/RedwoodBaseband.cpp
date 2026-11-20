/*
 * RedwoodBaseband (r5q Test 355): the IOKit services iOS's telephony stack opens, as listening stubs.
 *
 * CommCenter speaks QMI to its baseband (wifi/CELLULAR-PLAN.md): on the iPhone through Apple's AppleBasebandPCI driver,
 * and this phone's modem offers the standard QMI services over QRTR. The donor port saw CommCenter die on two missing
 * drivers -- SIGSEGV after "Could not find AppleIPAppender service", SIGABRT after "Unable to communicate with
 * AppleBaseband driver" (libTelephonyBasebandDynamic's TelephonyBasebandController) -- and Test 354 saw the same here.
 *
 * Here, under the class names iOS matches, are services that answer every call with success and zeroed outputs, and
 * log each call (the process, the selector, its scalars, the sizes and the first bytes of its structures): what
 * CommCenter asks of them, in order, before anything is made to really answer. Boot-arg bbstub=1 publishes them.
 *   AppleBaseband              TelephonyBasebandController: open, self-identify, state, properties, reset ...
 *   AppleIPAppender            the PDP helper (data contexts, flows)
 *   AppleBasebandPCIControl    libIOABP's IOABPControlClass: boot stage, logs, events
 *   AppleBasebandPCIInterface  libIOABP's IOABPClass, one per channel (ABPInterfaceProtocol): open, read, write ...
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSString.h>

extern "C" {
#include <pexpert/pexpert.h>
int proc_selfpid(void);
void proc_selfname(char * buf, int size);
void loader_log_putc(char c);
extern uint64_t redwood_bb_answer[64];  /* (Test 358) bsd/dev/redwood_wifi.c, kern.redwood.bb_answer */
}

extern "C" void RedwoodBasebandPublish(void);

/*
 * (Test 357) A line into the loader's log ring (what the PC's follower reads: loader_log_putc, as redwood_sec_ts.c's
 * rxlog) and the kernel's message buffer: IOLog and printf alone reach only the latter (Tests 355-356 lost the calls).
 */
static void
bblog(const char * format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);
}

/* ---- the stubs' user client: every call logged, success, outputs zeroed ---------------------------------------- */

class RedwoodBasebandUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(RedwoodBasebandUserClient);

	IOService *     fOwner;
	uint32_t        fCalls;

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodBasebandUserClient, IOUserClient);

bool
RedwoodBasebandUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	char procname[32] = "?";

	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	proc_selfname(procname, sizeof(procname));
	bblog("RedwoodBaseband: %s[%d] opens a connection, type %u\n", procname, proc_selfpid(), (unsigned) type);
	return true;
}

bool
RedwoodBasebandUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = provider;
	bblog("RedwoodBaseband: ... to %s (%s)\n", provider->getName(), provider->getMetaClass()->getClassName());
	return true;
}

IOReturn
RedwoodBasebandUserClient::clientClose(void)
{
	bblog("RedwoodBaseband: the connection to %s closed after %u calls\n", fOwner ? fOwner->getName() : "?", fCalls);
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

IOReturn
RedwoodBasebandUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	bblog("RedwoodBaseband: %s: a notification port (type %u, refcon 0x%llx)\n", fOwner ? fOwner->getName() : "?",
	    (unsigned) type, (unsigned long long) refCon);
	if (port != NULL) {
		releaseNotificationPort(port);
	}
	return kIOReturnSuccess;
}

IOReturn
RedwoodBasebandUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	const uint8_t * s = (const uint8_t *) args->structureInput;
	char procname[32] = "?", hex[3 * 24 + 1] = "";

	fCalls++;
	if (fCalls <= 300 || fCalls % 200 == 0) {
		for (uint32_t i = 0, n = 0; s != NULL && i < args->structureInputSize && i < 24; i++) {
			n += snprintf(hex + n, sizeof(hex) - n, "%02x ", s[i]);
		}
		proc_selfname(procname, sizeof(procname));
		bblog("RedwoodBaseband: %s[%d] -> %s: call %u selector %u%s (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in [%s], %u out%s)\n", procname, proc_selfpid(), fOwner ? fOwner->getName() : "?", fCalls, selector,
		    args->asyncWakePort != MACH_PORT_NULL ? " ASYNC" : "", args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, hex, args->structureOutputSize,
		    args->structureOutputDescriptor != NULL ? ", by descriptor" : "");
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i == 0 && selector < 64 ? redwood_bb_answer[selector] : 0;
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* ---- the services, under the class names iOS matches ------------------------------------------------------------ */

#define REDWOOD_BB_STUB(name) \
	class name : public IOService { OSDeclareDefaultStructors(name); }; \
	OSDefineMetaClassAndStructors(name, IOService)

REDWOOD_BB_STUB(AppleBaseband);
REDWOOD_BB_STUB(AppleIPAppender);
REDWOOD_BB_STUB(AppleBasebandPCIControl);
REDWOOD_BB_STUB(AppleBasebandPCIInterface);

static bool
publish(IOService * made, const char * name, const char * protocol)
{
	IOService * platform = IOService::getPlatform();

	if (made == NULL || platform == NULL || !made->init()) {
		OSSafeReleaseNULL(made);
		return false;
	}
	made->setName(name);
	made->setProperty("IOUserClientClass", "RedwoodBasebandUserClient");     /* (kIOUserClientClassKey) */
	if (protocol != NULL) {
		made->setProperty("ABPInterfaceProtocol", protocol);
	}
	if (!made->attach(platform)) {
		made->release();
		return false;
	}
	if (!made->start(platform)) {
		made->detach(platform);
		made->release();
		return false;
	}
	made->registerService();
	bblog("RedwoodBaseband: %s (%s%s%s) published\n", name, made->getMetaClass()->getClassName(), protocol ? ", " : "",
	    protocol ? protocol : "");
	return true;
}

void
RedwoodBasebandPublish(void)
{
	static const char * const channels[] = { "qmux_control", "qmux_loc", "qmux_extra", "qmux_logchannel", "qmux_test" };
	static bool done;
	uint32_t on = 0;

	if (done || !PE_parse_boot_argn("bbstub", &on, sizeof(on)) || on == 0) {
		return;
	}
	done = true;
	(void) publish(OSTypeAlloc(AppleBaseband), "AppleBaseband", NULL);
	(void) publish(OSTypeAlloc(AppleIPAppender), "AppleIPAppender", NULL);
	(void) publish(OSTypeAlloc(AppleBasebandPCIControl), "AppleBasebandPCIControl", NULL);
	for (unsigned i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
		(void) publish(OSTypeAlloc(AppleBasebandPCIInterface), "AppleBasebandPCIInterface", channels[i]);
	}
}
