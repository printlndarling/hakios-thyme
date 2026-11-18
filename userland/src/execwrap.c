/*
 * execwrap.c: stand in for an app's executable, keep its words, and exec the real binary.
 *
 * Why: an application launched by SpringBoard on this port aborts at once (Alpimaps:
 * SIGABRT, while the same binary runs fine as uid 501 outside SpringBoard), and an App's
 * stderr goes to /dev/null, so the exception is invisible.  Installed as the bundle's
 * executable (signed with the app's own entitlements, the real binary moved aside as
 * <name>.real), this wrapper appends everything the app says to /private/var/tmp/execwrap.err
 * before exec'ing the real one -- same pid, same launch job, same crash.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char real[4096];
	int fd;

	if (argc < 1 || argv[0] == NULL) {
		return 2;
	}
	snprintf(real, sizeof real, "%s.real", argv[0]);
	fd = open("/private/var/tmp/execwrap.err", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2) {
			close(fd);
		}
	}
	fprintf(stderr, "execwrap: uid=%d euid=%d self=%s real=%s\n", getuid(), geteuid(),
	    argv[0], real);
	execv(real, argv);
	fprintf(stderr, "execwrap: execv %s: %s\n", real, strerror(errno));
	return 127;
}
