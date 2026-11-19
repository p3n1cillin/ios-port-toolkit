/*
 * r5q_bluetool: a stand-in for Apple's BlueTool (the XPC service com.apple.BlueTool), for Bluetooth stage 4 (Test 256).
 *
 * On an iPhone, bluetoothd asks BlueTool to "boot", "init" and "deepsleep" the Broadcom chip (XPC kMsgId "RunCommand",
 * command = the script's name) and reads `result` (0 = done), `firmware` and `manufacturer` from the reply
 * (wifi/BLUETOOTH-IOS-RESEARCH.md section 2.1). Here the kernel has already powered the WCN3990 and loaded its firmware
 * (redwood_wifi.c, Bluetooth stages 2-3), and the real BlueTool knows only Broadcom firmware compiled into itself, so
 * this answers every request "done" at once. The reply's shape was inferred from bluetoothd's strings, not traced:
 * the three keys are put both at the top level and in a kMsgArgs dictionary. Every message is said on stderr (the
 * job's stderr is /dev/console, so it lands in the kernel log).
 *
 * Built by pipeline/build_root_app.sh into /redwood/bin; mkjobs.py runs it as com.apple.BlueTool (RELOCATE).
 */
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *xpc_object_t;
typedef void *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
extern xpc_object_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t q, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_object_t c, xpc_handler_t h);
extern void xpc_connection_resume(xpc_object_t c);
extern void xpc_connection_send_message(xpc_object_t c, xpc_object_t m);
extern xpc_type_t xpc_get_type(xpc_object_t o);
extern char *xpc_copy_description(xpc_object_t o);
extern const char *xpc_dictionary_get_string(xpc_object_t d, const char *k);
extern xpc_object_t xpc_dictionary_get_value(xpc_object_t d, const char *k);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t d);
extern xpc_object_t xpc_dictionary_create(const char *const *k, const xpc_object_t *v, size_t n);
extern void xpc_dictionary_set_int64(xpc_object_t d, const char *k, int64_t v);
extern void xpc_dictionary_set_string(xpc_object_t d, const char *k, const char *v);
extern void xpc_dictionary_set_value(xpc_object_t d, const char *k, xpc_object_t v);
extern void xpc_release(xpc_object_t o);
#define XPC_CONNECTION_MACH_SERVICE_LISTENER 1

static const char *firmware = "QCA WCN3990 crbtfw21 (loaded by the kernel)";
static int64_t manufacturer = 29;              /* Qualcomm */

static void
fill(xpc_object_t d)
{
	xpc_dictionary_set_int64(d, "result", 0);
	xpc_dictionary_set_string(d, "firmware", firmware);
	xpc_dictionary_set_int64(d, "manufacturer", manufacturer);
}

static void
serve(xpc_object_t peer)
{
	xpc_connection_set_event_handler(peer, ^(xpc_object_t msg) {
		if (xpc_get_type(msg) != (xpc_type_t)&_xpc_type_dictionary) {
			fprintf(stderr, "r5q_bluetool: peer event %s\n", msg == NULL ? "?" : "(not a message: closed)");
			return;
		}
		char *text = xpc_copy_description(msg);
		const char *id = xpc_dictionary_get_string(msg, "kMsgId");
		xpc_object_t reply = xpc_dictionary_create_reply(msg), args;

		fprintf(stderr, "r5q_bluetool: message %s: %s\n", id ? id : "(no kMsgId)", text ? text : "");
		free(text);
		if (reply == NULL) {
			return;
		}
		fill(reply);
		args = xpc_dictionary_create(NULL, NULL, 0);
		fill(args);
		xpc_dictionary_set_value(reply, "kMsgArgs", args);
		xpc_release(args);
		xpc_connection_send_message(peer, reply);
		xpc_release(reply);
		fprintf(stderr, "r5q_bluetool: answered result 0, manufacturer %lld\n", (long long)manufacturer);
	});
	xpc_connection_resume(peer);
}

int
main(void)
{
	const char *m = getenv("R5Q_BT_MANUFACTURER");
	xpc_object_t listener;

	if (m != NULL) {
		manufacturer = strtoll(m, NULL, 0);
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	listener = xpc_connection_create_mach_service("com.apple.BlueTool", dispatch_get_main_queue(),
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);
	fprintf(stderr, "r5q_bluetool: pid %d serving com.apple.BlueTool (every command answered \"done\")\n", getpid());
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) == (xpc_type_t)&_xpc_type_connection) {
			serve(peer);
		}
	});
	xpc_connection_resume(listener);
	dispatch_main();
}
