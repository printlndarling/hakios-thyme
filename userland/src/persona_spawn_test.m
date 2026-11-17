/*
 * persona_spawn_test: does a process running as the mobile user become root through a spawn
 * persona, the way TrollStore's app turns its helper into root (TSUtil spawnRoot:
 * posix_spawnattr_set_persona_np(POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE, 99) with persona uid/gid 0)?
 *
 * This is the one link in the store's UI path not yet measured: the helper works when a root
 * scene runs it (rc.jobs.tstore), but the app is uid 501 and relies on the persona.  The test
 * drops to 501 first, then spawns /usr/bin/id with the persona and prints what came back.
 *
 * usage: persona_spawn_test            spawn as uid 501 with persona 99 uid/gid 0
 *        persona_spawn_test uid501     just run id as uid 501, no persona (the control)
 */
#import <Foundation/Foundation.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include <string.h>
#include <errno.h>

typedef int (*PersonaFn)(posix_spawnattr_t *, uint32_t, int);
typedef int (*PersonaIdFn)(posix_spawnattr_t *, unsigned int);

int
main(int argc, char *argv[])
{
	@autoreleasepool {
		BOOL withPersona = !(argc > 1 && strcmp(argv[1], "uid501") == 0);
		const char *program = "/var/jb/usr/bin/id";
		PersonaFn setPersona = (PersonaFn)dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_np");
		PersonaIdFn setPersonaUid = (PersonaIdFn)dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_uid_np");
		PersonaIdFn setPersonaGid = (PersonaIdFn)dlsym(RTLD_DEFAULT, "posix_spawnattr_set_persona_gid_np");
		posix_spawnattr_t attributes;
		int status = 0, result;
		pid_t child;

		printf("persona_spawn_test: persona %s, set_persona_np %s\n", withPersona ? "on" : "off",
		    setPersona != NULL ? "present" : "missing");
		if (setuid(501) != 0 || setgid(501) != 0) {
			printf("persona_spawn_test: cannot become 501: %s\n", strerror(errno));
			return 1;
		}
		printf("persona_spawn_test: running as uid %u\n", getuid());

		posix_spawnattr_init(&attributes);
		if (withPersona) {
			if (setPersona == NULL || setPersonaUid == NULL || setPersonaGid == NULL) {
				printf("persona_spawn_test: the persona functions are not in libSystem\n");
				return 2;
			}
			result = setPersona(&attributes, 99, 1 /* POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE */);
			result |= setPersonaUid(&attributes, 0);
			result |= setPersonaGid(&attributes, 0);
			printf("persona_spawn_test: attributes set: %d\n", result);
		}
		char *const arguments[] = { (char *)program, (char *)NULL };
		extern char **environ;
		result = posix_spawn(&child, program, NULL, &attributes, arguments, environ);
		printf("persona_spawn_test: posix_spawn %s (%d)\n", result == 0 ? "ok" : "failed", result);
		if (result != 0) {
			return 3;
		}
		if (waitpid(child, &status, 0) == child) {
			printf("persona_spawn_test: child exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		}
		posix_spawnattr_destroy(&attributes);
	}
	return 0;
}
