/*
 * r5qd (Test 156): changes to a running iOS without a reboot. A root daemon (started by com.redwood.demo from
 * etc/rc.jobs) listening on TCP port 7777; every request is one line "TOKEN COMMAND ARGS...", the token being the one in
 * /redwood/share/r5qd/token (made at build time; the PC has a copy), compared in constant time; only peers on a private
 * network (10/8, 172.16/12, 192.168/16, 127/8) are served. Commands:
 *
 *   put PATH SIZE      then SIZE bytes: the file written (parents made), mode 0755           -> "ok"
 *   run PROG ARGS...   PROG started with ARGS (no shell), its output (64 KB at most, 120 s)  -> output, "exit N"
 *   kill NAME [SIG]    every process named NAME sent SIG (default 15): launchd restarts jobs  -> "killed N"
 *   inject [CONF]      kern.exec_inject read, or set to CONF ("LIB NAME...;LIB NAME...")       -> the setting
 *   ping                                                                                       -> "pong"
 *
 * The PC side is `python phone.py r5qd ...` (or wifi/r5qd_client.py). What it is for: an injected library rebuilt on
 * the PC, put into /var/r5q/lib, named in kern.exec_inject, and the program killed -- launchd starts it again with
 * the new library: seconds, not a four-minute rebuild-flash-reboot cycle. (Kernel changes still need a reboot.)
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>

extern char **environ;

#define PORT            7777
#define TOKEN_PATH      "/redwood/share/r5qd/token"
#define MAX_LINE        4096
#define MAX_OUTPUT      (64 * 1024)

static char token[128];

static void
reply(int s, const char *text)
{
	size_t n = strlen(text);

	while (n > 0) {
		ssize_t w = write(s, text, n);

		if (w <= 0) {
			return;
		}
		text += w;
		n -= (size_t)w;
	}
}

static int
read_line(int s, char *line, size_t size)
{
	size_t k = 0;

	while (k + 1 < size) {
		char c;
		ssize_t r = read(s, &c, 1);

		if (r <= 0) {
			return -1;
		}
		if (c == '\n') {
			break;
		}
		line[k++] = c;
	}
	line[k] = 0;
	return (int)k;
}

static int
token_ok(const char *given)
{
	size_t a = strlen(token), b = strlen(given);
	unsigned diff = (unsigned)(a ^ b);

	for (size_t i = 0; i < a; i++) {
		diff |= (unsigned)(token[i] ^ (i < b ? given[i] : 0));
	}
	return a >= 16 && diff == 0;
}

static int
private_peer(const struct sockaddr_in *peer)
{
	uint32_t a = ntohl(peer->sin_addr.s_addr);

	return (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == 0xac1 || (a >> 16) == 0xc0a8;
}

static void
make_parents(const char *path)
{
	char p[1024];

	strlcpy(p, path, sizeof(p));
	for (char *c = p + 1; *c; c++) {
		if (*c == '/') {
			*c = 0;
			mkdir(p, 0755);
			*c = '/';
		}
	}
}

static void
do_put(int s, char *args)
{
	char *path = strtok(args, " "), *size_text = strtok(NULL, " ");
	long size = size_text ? atol(size_text) : -1;
	char tmp[1100], buf[65536];
	int fd;

	if (path == NULL || path[0] != '/' || size < 0 || size > (512L << 20)) {
		reply(s, "error: put PATH SIZE\n");
		return;
	}
	make_parents(path);
	snprintf(tmp, sizeof(tmp), "%s.r5qd-new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0) {
		reply(s, "error: cannot write there\n");
		return;
	}
	while (size > 0) {
		ssize_t r = read(s, buf, size < (long)sizeof(buf) ? (size_t)size : sizeof(buf));

		if (r <= 0 || write(fd, buf, (size_t)r) != r) {
			close(fd);
			unlink(tmp);
			reply(s, "error: short\n");
			return;
		}
		size -= r;
	}
	fchmod(fd, 0755);
	close(fd);
	if (rename(tmp, path) != 0) {   /* (a new file in place of the old one: a program using the old one keeps it) */
		reply(s, "error: rename\n");
		return;
	}
	reply(s, "ok\n");
}

static void
do_run(int s, char *args)
{
	char *argv[64];
	int argc = 0, out[2];
	pid_t pid;
	posix_spawn_file_actions_t fa;
	char buf[4096], tail[64];
	size_t total = 0;
	int status = 0;
	time_t began = time(NULL);

	for (char *w = strtok(args, " "); w != NULL && argc < 63; w = strtok(NULL, " ")) {
		argv[argc++] = w;
	}
	argv[argc] = NULL;
	if (argc == 0 || pipe(out) != 0) {
		reply(s, "error: run PROG ARGS\n");
		return;
	}
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	posix_spawn_file_actions_adddup2(&fa, out[1], 2);
	posix_spawn_file_actions_addclose(&fa, out[0]);
	int err = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);

	if (err != 0) {
		char why[160];

		posix_spawn_file_actions_destroy(&fa);
		close(out[0]);
		close(out[1]);
		snprintf(why, sizeof(why), "error: cannot start %s: %s (%d)", argv[0], strerror(err), err);
		reply(s, why);
		reply(s, "\n");
		return;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	for (;;) {
		ssize_t r = read(out[0], buf, sizeof(buf));

		if (r > 0) {
			if (total < MAX_OUTPUT) {
				write(s, buf, (size_t)r);
			}
			total += (size_t)r;
			continue;
		}
		if (r == 0) {
			break;
		}
		if (waitpid(pid, &status, WNOHANG) == pid) {
			pid = 0;
			continue;       /* (what is left in the pipe is still read) */
		}
		if (time(NULL) - began > 120) {
			kill(pid, SIGKILL);
			reply(s, "\n(killed after 120 s)\n");
			break;
		}
		usleep(20000);
	}
	close(out[0]);
	if (pid != 0) {
		waitpid(pid, &status, 0);
	}
	snprintf(tail, sizeof(tail), "\nexit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
	reply(s, tail);
}

static void
do_kill(int s, char *args)
{
	char *name = strtok(args, " "), *sig_text = strtok(NULL, " ");
	int sig = sig_text ? atoi(sig_text) : SIGTERM, n = 0;
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
	size_t size = 0;
	struct kinfo_proc *procs;
	char text[64];

	if (name == NULL) {
		reply(s, "error: kill NAME [SIG]\n");
		return;
	}
	if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || (procs = malloc(size + 16 * sizeof(*procs))) == NULL) {
		reply(s, "error: no process list\n");
		return;
	}
	size += 16 * sizeof(*procs);
	if (sysctl(mib, 4, procs, &size, NULL, 0) == 0) {
		for (size_t i = 0; i < size / sizeof(*procs); i++) {
			pid_t pid = procs[i].kp_proc.p_pid;

			if (pid > 1 && strncmp(procs[i].kp_proc.p_comm, name, MAXCOMLEN) == 0 && kill(pid, sig) == 0) {
				n++;
			}
		}
	}
	free(procs);
	snprintf(text, sizeof(text), "killed %d\n", n);
	reply(s, text);
}

static void
do_inject(int s, char *args)
{
	char now[2100];
	size_t len = sizeof(now);

	if (args != NULL && *args != 0) {
		if (sysctlbyname("kern.exec_inject", NULL, NULL, args, strlen(args) + 1) != 0) {
			reply(s, "error: the kernel refused it (too long?)\n");
			return;
		}
	}
	if (sysctlbyname("kern.exec_inject", now, &len, NULL, 0) != 0) {
		reply(s, "error: cannot read it\n");
		return;
	}
	reply(s, now);
	reply(s, "\n");
}

static void
say_addresses(void)
{
	struct ifaddrs *list, *a;

	if (getifaddrs(&list) != 0) {
		return;
	}
	for (a = list; a != NULL; a = a->ifa_next) {
		if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, "lo0") != 0) {
			char ip[32];

			inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr, ip, sizeof(ip));
			printf("r5qd: listening on %s port %d (%s)\n", ip, PORT, a->ifa_name);
		}
	}
	freeifaddrs(list);
}

int
main(void)
{
	int fd = open(TOKEN_PATH, O_RDONLY), ls, one = 1;
	struct sockaddr_in addr;
	ssize_t n;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_DFL);
	if (fd < 0 || (n = read(fd, token, sizeof(token) - 1)) < 16) {
		printf("r5qd: no token in %s: not started\n", TOKEN_PATH);
		return 1;
	}
	close(fd);
	token[n] = 0;
	token[strcspn(token, "\r\n ")] = 0;
	ls = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(PORT);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
		printf("r5qd: cannot listen on port %d: %s\n", PORT, strerror(errno));
		return 1;
	}
	for (int i = 0; i < 12; i++) {          /* (the address comes with DHCP: said again for a minute) */
		say_addresses();
		if (i == 0) {
			printf("r5qd: up\n");
		}
		struct timeval tv = { 5, 0 };
		fd_set r;

		FD_ZERO(&r);
		FD_SET(ls, &r);
		if (select(ls + 1, &r, NULL, NULL, &tv) > 0) {
			break;
		}
	}
	for (;;) {
		struct sockaddr_in peer;
		socklen_t plen = sizeof(peer);
		int s = accept(ls, (struct sockaddr *)&peer, &plen);
		char line[MAX_LINE], *given, *cmd, *args;
		struct timeval tv = { 30, 0 };

		if (s < 0) {
			continue;
		}
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		if (!private_peer(&peer) || read_line(s, line, sizeof(line)) < 0) {
			close(s);
			continue;
		}
		given = strtok(line, " ");
		cmd = strtok(NULL, " ");
		args = strtok(NULL, "");
		if (given == NULL || cmd == NULL || !token_ok(given)) {
			reply(s, "error: no\n");
			close(s);
			continue;
		}
		if (strcmp(cmd, "ping") == 0) {
			reply(s, "pong\n");
		} else if (strcmp(cmd, "put") == 0) {
			do_put(s, args ? args : (char *)"");
		} else if (strcmp(cmd, "run") == 0) {
			do_run(s, args ? args : (char *)"");
		} else if (strcmp(cmd, "kill") == 0) {
			do_kill(s, args ? args : (char *)"");
		} else if (strcmp(cmd, "inject") == 0) {
			do_inject(s, args);
		} else {
			reply(s, "error: put, run, kill, inject, ping\n");
		}
		close(s);
	}
}
