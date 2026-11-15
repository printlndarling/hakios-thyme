/*
 * persona_test: does this kernel let us create the personas launchd keeps
 * looking for?
 *
 * launchd's log is full of
 *
 *   kpersona_find_by_type() failed with ret -1 errno 3 - No such process
 *   Unable to find persona with type 6
 *
 * -- PERSONA_SYSTEM_PROXY, the persona system daemons run under.  The persona
 * syscalls are gated by the com.apple.private.persona-mgmt entitlement
 * (bsd/kern/sys_persona.c), which a program only carries if it is signed with
 * one: this one is (src/persona_test.entitlements, ldid -S in build.sh).
 *
 *   persona_test          current persona, then find type 6/4/5
 *   persona_test alloc    ... and allocate the system-proxy, system and
 *                         default personas if they are not there
 *
 * The struct layout is the kernel's (bsd/sys/persona.h): 348 bytes, packed.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PERSONA_MAXLOGNAME 255
#define PERSONA_NGROUPS    16

struct kpersona_info {
	uint32_t persona_info_version;
	uid_t persona_id;
	int persona_type;
	gid_t persona_gid;
	uint32_t persona_ngroups;
	gid_t persona_groups[PERSONA_NGROUPS];
	uid_t persona_gmuid;
	char persona_name[PERSONA_MAXLOGNAME + 1];
	uid_t persona_uid;
} __attribute__((packed));

extern int kpersona_get(uid_t *id);
extern int kpersona_info(uid_t id, struct kpersona_info *info);
extern int kpersona_alloc(struct kpersona_info *info, uid_t *id);
extern int kpersona_find_by_type(int type, uid_t *id, size_t *idlen);
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);

#define CS_OPS_ENTITLEMENTS_BLOB 7

/* what the kernel loaded from this program's signature: the blob is a
 * CS_GenericBlob header (magic, length) followed by the XML property list */
static void
show_entitlements(void)
{
	char blob[4096];
	uint32_t length;
	int result;
	int error;

	errno = 0;
	result = csops(getpid(), CS_OPS_ENTITLEMENTS_BLOB, blob, sizeof(blob));
	error = errno;
	printf("persona_test: csops(entitlements): %d, errno %d (%s)\n", result, error,
	    strerror(error));
	if (result == 0 && sizeof(blob) >= 8) {
		memcpy(&length, blob + 4, sizeof(length));
		if (length > 8 && length <= sizeof(blob)) {
			fwrite(blob + 8, 1, length - 8, stdout);
			printf("\n");
		} else {
			printf("persona_test: entitlements length %u\n", (unsigned)length);
		}
	}
	fflush(stdout);
}

static void
report(const char *what, int result, uid_t id, int error)
{
	if (result == 0) {
		printf("persona_test: %s: ok, id %u\n", what, (unsigned)id);
	} else {
		printf("persona_test: %s: returned %d, errno %d (%s)\n", what, result, error,
		    strerror(error));
	}
	fflush(stdout);
}

static void
find_type(const char *what, int type)
{
	uid_t id = (uid_t)-1;
	size_t idlen = 1;
	int result = kpersona_find_by_type(type, &id, &idlen);
	int error = errno;

	report(what, result, id, error);
	if (result == 0) {
		struct kpersona_info info;

		memset(&info, 0, sizeof(info));
		info.persona_info_version = 2;
		if (kpersona_info(id, &info) == 0) {
			printf("persona_test: %s: name '%s' type %d uid %u\n", what,
			    info.persona_name, info.persona_type, (unsigned)info.persona_uid);
			fflush(stdout);
		}
	}
}

static void
alloc_persona(const char *name, int type, uid_t uid)
{
	struct kpersona_info info;
	uid_t id = (uid_t)-1;
	int result;

	memset(&info, 0, sizeof(info));
	info.persona_info_version = 2;
	info.persona_type = type;
	info.persona_uid = uid;
	strlcpy(info.persona_name, name, sizeof(info.persona_name));
	result = kpersona_alloc(&info, &id);
	report(name, result, id, errno);
}

int
main(int argc, char **argv)
{
	uid_t current = (uid_t)-1;
	int result;
	int error;

	if (argc > 1 && strcmp(argv[1], "csops") == 0) {
		show_entitlements();
		return 0;
	}
	result = kpersona_get(&current);
	error = errno;
	report("current thread persona", result, current, error);
	find_type("find type 6 (system proxy)", 6);
	find_type("find type 4 (system)", 4);
	find_type("find type 5 (default)", 5);

	if (argc > 1 && strcmp(argv[1], "alloc") == 0) {
		alloc_persona("systemproxy", 6, 0);
		alloc_persona("system", 4, 0);
		alloc_persona("mobile", 5, 501);
		find_type("find type 6 after alloc", 6);
		find_type("find type 4 after alloc", 4);
		find_type("find type 5 after alloc", 5);
	}
	return 0;
}
