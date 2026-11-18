/*
 * libr5q-nildict.dylib (diagnostic): -[__NSPlaceholderDictionary initWithObjects:forKeys:count:] with a nil object
 * says which keys were given and which object was nil, and leaves that pair out instead of throwing.
 * (r5q, Test 174: PosterBoard dies with "attempt to insert nil object from objects[1]" when the wallpapers are shown.)
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef id (*init_fn)(id, SEL, const id *, const id *, unsigned long);
static init_fn original;

static void
say(const char *text)
{
	write(2, text, strlen(text));
}

static const char *
describe(id o, char *buf, size_t size)
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
checked_init(id self, SEL sel, const id *objects, const id *keys, unsigned long count)
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
		return original(self, sel, objects, keys, count);
	}
	snprintf(line, sizeof(line), "r5q-nildict: %s[%d]: a dictionary of %lu with %lu nil:\n", getprogname(), getpid(), count, nils);
	say(line);
	for (unsigned long i = 0; i < count; i++) {
		snprintf(line, sizeof(line), "r5q-nildict:   [%lu] %s = %s\n", i, describe(keys[i], kb, sizeof(kb)),
		    describe(objects[i], ob, sizeof(ob)));
		say(line);
		if (objects[i] != NULL && keys[i] != NULL) {
			o[n] = objects[i];
			k[n] = keys[i];
			n++;
		}
	}
	return original(self, sel, o, k, n);
}

__attribute__((constructor)) static void
start(void)
{
	Class c = objc_getClass("__NSPlaceholderDictionary");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("initWithObjects:forKeys:count:")) : NULL;

	if (m != NULL) {
		original = (init_fn)method_setImplementation(m, (IMP)checked_init);
		say("r5q-nildict: watching dictionaries made with a nil object\n");
	}
}
