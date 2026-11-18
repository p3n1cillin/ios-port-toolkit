/* sym_probe LIB SYMBOL...: where symbols are in this process (the shared cache's addresses, unslid) */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>

extern intptr_t _dyld_get_image_vmaddr_slide(uint32_t image_index);
extern const void *_dyld_get_shared_cache_range(size_t *length);

int
main(int argc, char **argv)
{
	size_t length = 0;
	uintptr_t base = (uintptr_t)_dyld_get_shared_cache_range(&length);

	if (argc < 3 || dlopen(argv[1], RTLD_LAZY) == NULL) {
		printf("sym_probe: cannot open %s\n", argc > 1 ? argv[1] : "?");
		return 1;
	}
	printf("shared cache at %#lx (%zu bytes)\n", base, length);
	for (int i = 2; i < argc; i++) {
		void *p = dlsym(RTLD_DEFAULT, argv[i]);
		Dl_info info = { 0 };

		if (p != NULL) {
			dladdr(p, &info);
		}
		printf("%s: %p (cache offset %#lx) in %s\n", argv[i], p, p ? (uintptr_t)p - base : 0,
		    info.dli_fname ? info.dli_fname : "-");
	}
	return 0;
}
