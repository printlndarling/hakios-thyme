/*
 * killby: send a signal (SIGKILL unless told otherwise) to every process whose name is NAME.
 * The images have no killall; libproc finds the processes by name, which is enough for the
 * scenes (Sileo, Preferences) and for the owner's "terminated it and opened it again" tests.
 *
 *   killby NAME [SIGNAL]        (SIGNAL is a number: 9, 15, ...)
 */
#if __has_include(<libproc.h>)
#include <libproc.h>
#else
/* the SDK has libproc.tbd but not the header: the two calls this needs, named here */
#define PROC_ALL_PIDS 1
extern int proc_listpids(unsigned int type, unsigned int typeinfo, void *buffer, int buffersize);
extern int proc_name(int pid, void *buffer, unsigned int buffersize);
#endif
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	pid_t pids[4096];
	int signal_number = SIGKILL, count, found = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: killby NAME [SIGNAL]\n");
		return 2;
	}
	if (argc > 2) {
		signal_number = atoi(argv[2]);
	}
	count = proc_listpids(PROC_ALL_PIDS, 0, pids, (int)sizeof(pids));
	if (count <= 0) {
		fprintf(stderr, "killby: no process list (%d)\n", count);
		return 1;
	}
	for (int i = 0; i < count / (int)sizeof(pid_t); i++) {
		char name[64] = "";

		if (pids[i] <= 0 || proc_name(pids[i], name, sizeof(name)) <= 0) {
			continue;
		}
		if (strcmp(name, argv[1]) != 0) {
			continue;
		}
		if (kill(pids[i], signal_number) == 0) {
			printf("killby: %s: %d (signal %d)\n", name, (int)pids[i], signal_number);
			found++;
		} else {
			printf("killby: %s: %d: %s\n", name, (int)pids[i], strerror(errno));
		}
	}
	if (found == 0) {
		printf("killby: no process named %s\n", argv[1]);
	}
	return found == 0;
}
