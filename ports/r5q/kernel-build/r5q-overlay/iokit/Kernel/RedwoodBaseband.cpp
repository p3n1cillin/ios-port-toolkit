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
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSString.h>
#include <string.h>

extern "C" {
#include <pexpert/pexpert.h>
int proc_selfpid(void);
void proc_selfname(char * buf, int size);
void loader_log_putc(char c);
extern uint64_t redwood_bb_answer[64];  /* (Test 358) bsd/dev/redwood_wifi.c, kern.redwood.bb_answer */
/* (Test 380) bsd/dev/redwood_wifi.c: QMI to the modem's services over QRTR, one port per (connection, service, client) */
int redwood_bb_qmi_send(void * client, uint8_t service, uint8_t qmux_client, const void * message, size_t length);
void redwood_bb_qmi_close(void * client);
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
	task_t          fTask;
	/*
	 * (Test 379) the QMUX transport over an AppleBasebandPCIInterface: reads the client posts (async, its buffer and
	 * size) wait here until there is something for them; replies made before a read is posted wait in fOut.
	 */
	struct PendingRead { OSAsyncReference64 ref; mach_vm_address_t addr; uint32_t size; };
	PendingRead     fReads[16];
	uint32_t        fReadCount;
	/*
	 * (Test 382) replies waiting for reads: whole QMUX frames back to back (each says its length), up to 64 KB; a read
	 * takes as many whole frames as fit its buffer. (Test 381's 16 one-frame slots overflowed: CommCenter keeps only a
	 * few reads posted, and the modem's answers came faster -- 70 dropped, and their requests timed out.)
	 */
	enum { kOutSize = 65536, kFrameMax = 2048 };
	uint8_t *       fOut;
	uint32_t        fOutLen;
	uint8_t         fNextClient[256];

	void            qmuxWrite(const uint8_t * frame, uint32_t len);
	void            forwardToModem(const uint8_t * frame, uint32_t len);
	void            queueReply(const uint8_t * frame, uint32_t len);
	void            deliver(void);

public:
	void            fromModem(uint8_t svc, uint8_t client, const void * message, size_t length);
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(RedwoodBasebandUserClient, IOUserClient);

static IOLock * gBBLock;        /* (Test 379) the transport state of every connection: external methods run concurrently */

void
RedwoodBasebandUserClient::free(void)
{
	if (fOut != NULL) {
		IOFree(fOut, kOutSize);
		fOut = NULL;
	}
	IOUserClient::free();
}

bool
RedwoodBasebandUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	char procname[32] = "?";

	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	if (gBBLock == NULL) {
		IOLock * l = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, l, &gBBLock)) {
			IOLockFree(l);
		}
	}
	proc_selfname(procname, sizeof(procname));
	bblog("RedwoodBaseband: %s[%d] opens a connection, type %u\n", procname, proc_selfpid(), (unsigned) type);
	return true;
}

/* (Test 379) a reply frame into the client's next posted read, or kept until it posts one */
void
RedwoodBasebandUserClient::queueReply(const uint8_t * frame, uint32_t len)
{
	static SInt32 dropped;

	if (fOut == NULL) {
		fOut = (uint8_t *) IOMalloc(kOutSize);
		fOutLen = 0;
	}
	if (fOut == NULL || len > kFrameMax || fOutLen + len > kOutSize) {
		if (OSIncrementAtomic(&dropped) < 20) {
			bblog("RedwoodBaseband: qmux: reply of %u bytes dropped (%u bytes waiting)\n", len, fOutLen);
		}
		return;
	}
	bcopy(frame, fOut + fOutLen, len);
	fOutLen += len;
	deliver();
}

void
RedwoodBasebandUserClient::deliver(void)
{
	static SInt32 told;

	while (fOutLen > 0 && fReadCount > 0) {
		PendingRead r = fReads[0];
		uint32_t n = 0, frames = 0;
		IOMemoryDescriptor * md;
		IOReturn ret = kIOReturnNoMemory;

		/* whole frames (01, length u16 = the frame without the 01) while they fit the read */
		while (n + 3 <= fOutLen) {
			uint32_t f = 1 + (uint32_t) (fOut[n + 1] | fOut[n + 2] << 8);
			if (n + f > fOutLen || n + f > r.size) {
				break;
			}
			n += f;
			frames++;
		}
		if (n == 0) {           /* one frame bigger than the read: cut (should not happen: reads are 6 KB) */
			n = fOutLen < r.size ? fOutLen : r.size;
		}
		md = IOMemoryDescriptor::withAddressRange(r.addr, n, kIODirectionIn, fTask);
		if (md != NULL) {
			ret = md->prepare();
			if (ret == kIOReturnSuccess) {
				md->writeBytes(0, fOut, n);
				md->complete();
			}
			md->release();
		}
		io_user_reference_t done[2] = { n, 0 };
		memmove(&fReads[0], &fReads[1], (fReadCount - 1) * sizeof(fReads[0]));
		fReadCount--;
		memmove(fOut, fOut + n, fOutLen - n);
		fOutLen -= n;
		IOReturn sent = sendAsyncResult64(r.ref, ret, done, 2);
		/* (Test 383) every delivery of a non-CTL frame shown with its first bytes and the read's size (who reads it?) */
		if (OSIncrementAtomic(&told) < 400 && (fOut != NULL)) {
			char hex[3 * 16 + 1] = "";
			IOMemoryDescriptor * back = IOMemoryDescriptor::withAddressRange(r.addr, n < 16 ? n : 16, kIODirectionOut, fTask);
			uint8_t first[16] = { 0 };
			if (back != NULL) {
				if (back->prepare() == kIOReturnSuccess) {
					back->readBytes(0, first, n < 16 ? n : 16);
					back->complete();
				}
				back->release();
			}
			for (uint32_t i = 0, k = 0; i < 16 && i < n; i++) {
				k += snprintf(hex + k, sizeof(hex) - k, "%02x ", first[i]);
			}
			if (n >= 5 && first[4] != 0) {
				bblog("RedwoodBaseband: qmux: %u frames (%u bytes) into a read of %u (0x%x, sent 0x%x): %s\n", frames, n,
				    r.size, ret, sent, hex);
			}
		}
	}
}

/*
 * (Test 379) One QMUX frame from CommCenter (libPCITransport's write): 0x01, length (u16, the frame without the 0x01),
 * control flags, service, client, then the QMI message. QMI CTL (service 0) is answered here -- client IDs handed out
 * per service -- so the services' clients can start; the other services' messages are only logged for now (next: to
 * the modem's own QMI services over QRTR).
 */
void
RedwoodBasebandUserClient::qmuxWrite(const uint8_t * f, uint32_t len)
{
	if (len < 12 || f[0] != 0x01) {
		return;
	}
	uint8_t svc = f[4], client = f[5];
	const uint8_t * m = f + 6;      /* CTL: flags, txid (u8), msgid (u16), length (u16), TLVs; others: flags, txid (u16), ... */
	if (svc == 0) {
		uint8_t txid = m[1];
		uint16_t msgid = (uint16_t) (m[2] | m[3] << 8);
		uint8_t out[64];
		uint32_t o = 0, tlv_start;
		uint8_t wanted = 0;

		/* TLV 0x01 of GET_CLIENT_ID / RELEASE_CLIENT_ID: the service (and client) */
		for (uint32_t t = 12; t + 3 <= len; ) {
			uint16_t tl = (uint16_t) (f[t + 1] | f[t + 2] << 8);
			if (f[t] == 0x01 && tl >= 1 && t + 3 + tl <= len) {
				wanted = f[t + 3];
			}
			t += 3 + tl;
		}
		out[o++] = 0x01; o += 2;                                /* QMUX: I/F type, length (below) */
		out[o++] = 0x80; out[o++] = 0; out[o++] = 0;            /* from the service, CTL, client 0 */
		out[o++] = 0x01; out[o++] = txid;                       /* CTL response */
		out[o++] = (uint8_t) msgid; out[o++] = (uint8_t) (msgid >> 8);
		o += 2;                                                 /* TLV length (below) */
		tlv_start = o;
		out[o++] = 0x02; out[o++] = 4; out[o++] = 0;            /* result TLV: success */
		out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;
		if (msgid == 0x22 && wanted == 6) {
			/*
			 * (Test 388) No PDS client: locationd asks the baseband for GPS through PDS, and this modem's PDS answers
			 * neither as an iPhone's does (QMI_ERR_INVALID_INDEX) nor with the data locationd wants; every failure made
			 * locationd ask for a baseband reset (Tests 385-387). Refused: result 1, error 0x05 (client IDs exhausted).
			 */
			out[o - 4] = 1;
			out[o - 2] = 0x05;
			bblog("RedwoodBaseband: qmux: CTL GET_CLIENT_ID for PDS refused\n");
		} else if (msgid == 0x22) {                             /* GET_CLIENT_ID */
			uint8_t cid = ++fNextClient[wanted];
			out[o++] = 0x01; out[o++] = 2; out[o++] = 0;
			out[o++] = wanted; out[o++] = cid;
			bblog("RedwoodBaseband: qmux: CTL GET_CLIENT_ID for service 0x%x: client %u\n", wanted, cid);
		} else if (msgid == 0x23) {                             /* RELEASE_CLIENT_ID: echo it */
			out[o++] = 0x01; out[o++] = 2; out[o++] = 0;
			out[o++] = wanted; out[o++] = len >= 17 ? f[16] : 0;
		} else {
			bblog("RedwoodBaseband: qmux: CTL message 0x%x answered with success only\n", msgid);
		}
		out[10] = (uint8_t) (o - tlv_start); out[11] = (uint8_t) ((o - tlv_start) >> 8);
		out[1] = (uint8_t) (o - 1); out[2] = (uint8_t) ((o - 1) >> 8);
		queueReply(out, o);
		return;
	}
	/* (Test 380) any other service: to the modem (forwardToModem, after the transport's lock is let go) */
}

/*
 * (Test 380) A QMI message of service `svc`, QMUX client `client`, to the modem's server of that service over QRTR
 * (bsd/dev/redwood_wifi.c's bridge). Called without gBBLock: sending may wait for the link's thread, which takes
 * gBBLock to deliver what comes back. A service the modem has no server of is answered here with a QMI error
 * (result 1, error 0x31 "invalid service type"), so its client does not wait forever.
 */
void
RedwoodBasebandUserClient::forwardToModem(const uint8_t * f, uint32_t len)
{
	uint8_t svc = f[4], client = f[5];
	const uint8_t * m = f + 6;
	uint32_t mlen = len - 6;
	/*
	 * (Test 385) COEX (0x22) is kept from the modem: iOS's coexistence policy and Wi-Fi power limits went to it in
	 * Test 384 and the phone's Wi-Fi (whose firmware runs beside the modem here) was deauthenticated and stayed down.
	 */
	uint16_t msgid = (uint16_t) (m[3] | m[4] << 8);
	/*
	 * (Test 386) Answered here with success, not sent: PDS RESET (service 6, message 0) -- the modem answers it
	 * QMI_ERR_INVALID_INDEX and locationd then asks for a baseband reset (Test 385).
	 */
	bool local_ok = svc == 6;      /* (Test 387: every PDS request -- GET_GPS_SERVICE_VERSION failed the same way) */
	int error = local_ok ? 1 : svc == 0x22 ? 1 /* EPERM */ : redwood_bb_qmi_send(this, svc, client, m, mlen);
	static SInt32 told;

	if ((OSIncrementAtomic(&told) + 1) <= 120 || error != 0) {
		bblog("RedwoodBaseband: qmux: service 0x%x client %u message 0x%x (%u bytes) to the modem: %d\n", svc, client,
		    (unsigned) (m[3] | m[4] << 8), mlen, error);
	}
	if (error != 0 && mlen >= 7 && (m[0] & 0x06) == 0) {      /* a request: answer it with an error */
		/* (Test 384: length 19, the frame without the 01; 18 left a byte and broke the stream) */
		uint8_t out[20] = { 0x01, 19, 0, 0x80, svc, client, 0x02, m[1], m[2], m[3], m[4], 7, 0,
			                0x02, 4, 0, (uint8_t) (local_ok ? 0 : 1), 0x00, (uint8_t) (local_ok ? 0 : 0x31), 0x00 };
		IOLockLock(gBBLock);
		queueReply(out, sizeof(out));
		IOLockUnlock(gBBLock);
	}
}

/* (Test 380) the modem's QMI for (svc, client), from the link's thread: into QMUX, to the client's reads */
void
RedwoodBasebandUserClient::fromModem(uint8_t svc, uint8_t client, const void * message, size_t length)
{
	uint8_t frame[kFrameMax];
	static SInt32 told;

	if (length + 6 > sizeof(frame)) {
		bblog("RedwoodBaseband: qmux: %zu bytes from the modem for service 0x%x: too big, dropped\n", length, svc);
		return;
	}
	frame[0] = 0x01;
	frame[1] = (uint8_t) (length + 5);
	frame[2] = (uint8_t) ((length + 5) >> 8);
	frame[3] = 0x80;
	frame[4] = svc;
	frame[5] = client;
	bcopy(message, frame + 6, length);
	if ((OSIncrementAtomic(&told) + 1) <= 120) {
		const uint8_t * m = (const uint8_t *) message;
		bblog("RedwoodBaseband: qmux: from the modem, service 0x%x client %u: %s 0x%x (%zu bytes)\n", svc, client,
		    length > 0 && m[0] == 4 ? "indication" : "response", length >= 5 ? (unsigned) (m[3] | m[4] << 8) : 0, length);
	}
	IOLockLock(gBBLock);
	queueReply(frame, (uint32_t) (length + 6));
	IOLockUnlock(gBBLock);
}

extern "C" void
RedwoodBasebandQmiReceive(void * client, uint8_t service, uint8_t qmux_client, const void * message, size_t length)
{
	((RedwoodBasebandUserClient *) client)->fromModem(service, qmux_client, message, length);
}

bool
RedwoodBasebandUserClient::start(IOService * provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = provider;
	OSString * protocol = OSDynamicCast(OSString, provider->getProperty("ABPInterfaceProtocol"));
	bblog("RedwoodBaseband: ... to %s (%s%s%s)\n", provider->getName(), provider->getMetaClass()->getClassName(),
	    protocol ? ", " : "", protocol ? protocol->getCStringNoCopy() : "");
	return true;
}

IOReturn
RedwoodBasebandUserClient::clientClose(void)
{
	bblog("RedwoodBaseband: the connection to %s closed after %u calls\n", fOwner ? fOwner->getName() : "?", fCalls);
	redwood_bb_qmi_close(this);     /* (Test 380) its QRTR ports: no callback for this connection after this */
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
	/*
	 * (Test 378) libPCITransport starts each channel of an AppleBasebandPCIInterface with startChannelAsync (selector 7,
	 * scalar = channel 0 or 1) and waits for the completion on its async port; with none it times out (0xe00002d6),
	 * aborts (selector 6) and fails "to create transport internal" (Test 377). Start and abort are completed here at
	 * once with success; other async calls (reads, writes, maps) are only logged, not completed, for now.
	 */
	if (args->asyncWakePort != MACH_PORT_NULL && fOwner != NULL && strcmp(fOwner->getName(), "AppleBasebandPCIInterface") == 0 &&
	    (selector == 6 || selector == 7)) {
		IOReturn sent = sendAsyncResult64(args->asyncReference, kIOReturnSuccess, NULL, 0);
		bblog("RedwoodBaseband: ... selector %u completed (channel %llu): 0x%x\n", selector,
		    args->scalarInputCount > 0 ? in[0] : 0, sent);
	}
	/* (Test 379) read (3: buffer, size) waits for a reply; write (2: buffer, length) is read in, answered, completed */
	if (args->asyncWakePort != MACH_PORT_NULL && fOwner != NULL && strcmp(fOwner->getName(), "AppleBasebandPCIInterface") == 0 &&
	    args->scalarInputCount >= 2 && (selector == 2 || selector == 3) && gBBLock != NULL) {
		uint8_t frame[1024];
		uint32_t forward = 0;
		static SInt32 writes;

		IOLockLock(gBBLock);
		if (selector == 3) {
			if (fReadCount < sizeof(fReads) / sizeof(fReads[0])) {
				bcopy(args->asyncReference, fReads[fReadCount].ref, sizeof(OSAsyncReference64));
				fReads[fReadCount].addr = in[0];
				fReads[fReadCount].size = (uint32_t) in[1];
				fReadCount++;
				deliver();
			}
		} else {
			uint32_t n = in[1] < sizeof(frame) ? (uint32_t) in[1] : sizeof(frame);
			IOMemoryDescriptor * md = IOMemoryDescriptor::withAddressRange(in[0], n, kIODirectionOut, fTask);
			IOReturn ret = kIOReturnNoMemory;
			io_user_reference_t done[2] = { in[1], 0 };
			char line[3 * 32 + 1] = "";

			if (md != NULL) {
				ret = md->prepare();
				if (ret == kIOReturnSuccess) {
					md->readBytes(0, frame, n);
					md->complete();
				}
				md->release();
			}
			for (uint32_t i = 0, k = 0; ret == kIOReturnSuccess && i < n && i < 32; i++) {
				k += snprintf(line + k, sizeof(line) - k, "%02x ", frame[i]);
			}
			if ((OSIncrementAtomic(&writes) + 1) <= 200) {
				bblog("RedwoodBaseband: qmux: write of %u bytes: %s\n", n, line);
			}
			sendAsyncResult64(args->asyncReference, kIOReturnSuccess, done, 2);
			if (ret == kIOReturnSuccess && n >= 12 && frame[0] == 0x01) {
				if (frame[4] == 0) {
					qmuxWrite(frame, n);
				} else {
					forward = n;
				}
			}
		}
		IOLockUnlock(gBBLock);
		if (forward != 0) {
			forwardToModem(frame, forward);
		}
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
/*
 * (Test 374) libPCITransport's PCI event listener (what libBasebandManager's boot.mod waits on after a boot, "Could not
 * create PCI event listener!" without it) watches two services: AppleBasebandPCIControl (libIOABP's IOABPControlClass)
 * and the packet-data manager, matched as AppleBasebandPCIPDPManagerBase (its PDPListener).
 */
REDWOOD_BB_STUB(AppleBasebandPCIPDPManagerBase);

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
	(void) publish(OSTypeAlloc(AppleBasebandPCIPDPManagerBase), "AppleBasebandPCIPDPManagerBase", NULL);
	for (unsigned i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
		(void) publish(OSTypeAlloc(AppleBasebandPCIInterface), "AppleBasebandPCIInterface", channels[i]);
	}
}
