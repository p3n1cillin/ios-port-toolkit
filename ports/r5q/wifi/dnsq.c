/*
 * dnsq HOST... (2026-11-20): each host name resolved on the phone (getaddrinfo, as apps do), its addresses printed.
 * Why: Weather's requests failed with "Connection refused" to 0.0.0.0:443 -- a name answered with 0.0.0.0, which is
 * how DNS blocklists (ad blockers on a router or a Pi-hole) refuse a host. This says which names, and whether the
 * phone's own resolver or the network's answers that way.
 */
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
		int e = getaddrinfo(argv[i], "443", &hints, &res);

		printf("%-40s", argv[i]);
		if (e != 0) {
			printf(" error: %s\n", gai_strerror(e));
			continue;
		}
		for (struct addrinfo *a = res; a != NULL; a = a->ai_next) {
			char text[64] = "?";
			const void *addr = a->ai_family == AF_INET ? (const void *)&((struct sockaddr_in *)a->ai_addr)->sin_addr :
			    (const void *)&((struct sockaddr_in6 *)a->ai_addr)->sin6_addr;
			inet_ntop(a->ai_family, addr, text, sizeof(text));
			printf(" %s", text);
		}
		printf("\n");
		freeaddrinfo(res);
	}
	return 0;
}
