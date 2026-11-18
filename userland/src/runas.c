/*
 * runas.c: drop from root to a user and exec a program with stdout/stderr kept.
 *
 * Why: an application launched by SpringBoard aborts after install on this port (Alpimaps:
 * SIGABRT right after spawn, and its stderr goes nowhere), while the same binary started
 * from the scene as root runs -- it initialises NativeScript and reaches its event loop
 * (logs/182809-run.log).  The remaining difference is the environment the process starts
 * in: uid 501 and the launchd job, or nobody's.  This tool reproduces the first half of
 * that (uid, groups) outside SpringBoard and appends the program's own words to a file.
 *
 * usage: runas UID LOG PROG [ARGS...]
 */
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	uid_t uid;
	int fd;

	if (argc < 4) {
		fprintf(stderr, "usage: %s UID LOG PROG [ARGS...]\n", argv[0]);
		return 2;
	}
	uid = (uid_t)atoi(argv[1]);
	fd = open(argv[2], O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		perror("open log");
	}
	if (setgroups(0, NULL) != 0) {
		perror("setgroups");
	}
	if (setgid(uid) != 0) {
		perror("setgid");
	}
	if (setuid(uid) != 0) {
		perror("setuid");
	}
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2) {
			close(fd);
		}
	}
	execv(argv[3], &argv[3]);
	perror("execv");
	return 127;
}
