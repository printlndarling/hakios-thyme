/*
 * Stand-ins for the two closed security policies user space cannot do
 * without: AMFI and the Sandbox. On a device they are kexts that register
 * MAC policies of those names; system software talks to them through
 * __mac_syscall() and relies on what they decide.
 *
 * Neither stand-in is a security mechanism. This kernel runs whatever it is
 * given (see libkern/amfi/miniamfi.c); these only give the answers the Darwin
 * user space needs in order to run at all:
 *
 *   AMFI     The binaries of the operating system carry ad hoc signatures
 *            (no certificate chain); on a device AMFI finds their hashes
 *            in the trust cache and flags them as platform binaries.
 *            launchd panics if a system daemon is not one. There is no
 *            trust cache here, so every ad hoc signed binary is taken for
 *            a platform binary. Unsigned programs (our own) are not.
 *
 *   Sandbox  Every query (may this process look up that service?) is
 *            answered "allowed", and every request to apply a profile
 *            succeeds without applying anything. But WHETHER a process
 *            is sandboxed is answered as on a device: the stand-in keeps
 *            a list of the processes a profile was applied to (by the
 *            seatbelt-profiles entitlement at exec, by sandbox_init or
 *            sandbox_apply, inherited across fork, spawn and exec) and
 *            says so to sandbox_check(pid, NULL, 0). The other questions
 *            with answers of their own are which container a process is
 *            in and what an issued sandbox extension's token reads.
 *            (devices/xiaomi-mi-10/SANDBOX-umi.md)
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/codesign.h>
#include <sys/errno.h>
#include <sys/kpi_socket.h>
#include <sys/priv.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <sys/vnode_internal.h>
#include <libkern/libkern.h>
#include <kern/cs_blobs.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include <os/atomic_private.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <security/mac_framework.h>
#include <security/mac_policy.h>

void minimac_policy_init(void);

/*
 * The stand-ins' own log lines. printf() alone reaches only the kernel's message buffer once the
 * console has been handed over: the loader appends that buffer to the log only when a run comes
 * back to it, and a run read over USB while the kernel is alive ("KERNEL LOG (the kernel is still
 * running)", every run since 2026-11-10 with usb=1) never shows it. So every line also goes into
 * the loader's live log ring, as the drivers' lines do (osfmk/arm64/loader_return.c).
 */
extern void loader_log_line(const char *line);

static void minisandbox_say(const char *format, ...) __printflike(1, 2);

static void
minisandbox_say(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	printf("%s", line);
}

static int
miniamfi_vnode_check_signature(__unused struct vnode *vp, __unused struct label *label, __unused cpu_type_t cpu_type,
    struct cs_blob *cs_blob, unsigned int *cs_flags, __unused unsigned int *signer_type, __unused int flags,
    __unused unsigned int platform, __unused char **fatal_failure_desc, __unused size_t *fatal_failure_desc_len)
{
	if (cs_blob == NULL) {
		return 0;
	}
	/*
	 * Accepting a signature is what marks the process as signed; the
	 * kernel consults the signature of a process only when that is set.
	 */
	*cs_flags |= CS_SIGNED;
	if (*cs_flags & CS_ADHOC) {
		*cs_flags |= CS_PLATFORM_BINARY;
	}
	return 0;
}

/*
 * dyld asks AMFI what it may do for the process it is starting (request 0x5a, "check dyld
 * policy"): the argument is { flags in, pointer to flags out }. Everything is allowed --
 * @paths, the DYLD_* variables, inserted libraries and interposing -- which is what lets
 * our own library be loaded into stock daemons (DYLD_INSERT_LIBRARIES).
 */
#define AMFI_SYSCALL_CHECK_DYLD_POLICY  0x5a
#define AMFI_DYLD_OUTPUT_ALLOW_ALL      0xffULL

/*
 * Request 0x60, "query boot-arg state" (amfi_interface_query_bootarg_state() in libamfi): the
 * argument points to 64 bits of flags, one for each boot-arg that switches a part of AMFI off
 * (amfi_get_out_of_my_way and its kin). ExtensionKit asks, and unless one of the bits 0x86 is set
 * it wants the signature of the system's table of extension points
 * (/System/Library/xpc/extensions.plist.sig) verified by AMFI's IOKit service. There is no such
 * service here ("Unable to find AMFI"); the table is then not read and LaunchServices knows no
 * extension point: every application extension is refused when the applications are registered
 * ("does not correspond to a known extension point"), pkd cannot make a record for any extension
 * point, and Shortcuts dies on a nil one. Nothing is enforced by this kernel, so that is what it
 * says. (Which of the three bits stands for which boot-arg is not known; ExtensionKit takes any
 * of them.)
 */
#define AMFI_SYSCALL_QUERY_BOOTARG_STATE 0x60
#define AMFI_BOOTARG_STATE_NOT_ENFORCING 0x86ULL

static int
miniamfi_policy_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
	struct {
		uint64_t input_flags;
		user_addr_t output_flags;
	} args;
	uint64_t flags = AMFI_DYLD_OUTPUT_ALLOW_ALL;
	uint64_t state = AMFI_BOOTARG_STATE_NOT_ENFORCING;
	int error;

	if (call == AMFI_SYSCALL_QUERY_BOOTARG_STATE) {
		return copyout(&state, arg, sizeof(state));
	}
	if (call != AMFI_SYSCALL_CHECK_DYLD_POLICY) {
		return ENOPOLICY;               /* what user space got before there was a handler */
	}
	error = copyin(arg, &args, sizeof(args));
	if (error == 0) {
		error = copyout(&flags, args.output_flags, sizeof(flags));
	}
	return error;
}

static const struct mac_policy_ops miniamfi_ops = {
	.mpo_vnode_check_signature = miniamfi_vnode_check_signature,
	.mpo_policy_syscall = miniamfi_policy_syscall,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) miniamfi_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "Apple Mobile File Integrity (stand-in)",
	.mpc_ops = &miniamfi_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) miniamfi_handle;

/*
 * Request 4, "container path of a process": sandbox_container_path_for_pid() and
 * sandbox_container_path_for_audit_token() of libsystem_sandbox. The argument is
 * { pid, version of the pid, buffer, its size, whether the version is to be compared }.
 *
 * On a device the Sandbox learns the container when the program is executed (it asks
 * containermanagerd) and confines the process to it. Nothing is confined here, but the question
 * is still asked and the answer matters: an application extension hosted by PlugInKit does not
 * run without a container. It gives the buffer a "?" to start with and wants a longer string
 * back, else "plugin has invalid sandbox or container" (-[PKService checkEnvironment:]) -- the
 * end of every Intents extension while this request "succeeded" without writing anything.
 *
 * Whoever starts a program that lives in a container -- RunningBoard an application, pkd an
 * extension -- has had the path from containermanagerd and passes it in the environment as
 * CFFIXED_USER_HOME, which is where Foundation takes the home directory from. So that is the
 * answer: it is looked up among the strings exec left at the top of the stack of the process.
 * A process without the variable has no container and gets ENOENT, as an unsandboxed process
 * does on a device. Success with an untouched buffer is what this used to answer, and stock code
 * believes it: SafariCore (in com.apple.Safari.History, a launchd job with no CFFIXED_USER_HOME)
 * made an NSURL from the unwritten buffer and aborted ("nil string parameter") instead of taking
 * its own fallback, the data container of com.apple.mobilesafari from LaunchServices.
 */
#define SANDBOX_SYSCALL_CONTAINER_PATH  4
#define SANDBOX_CONTAINER_VARIABLE      "CFFIXED_USER_HOME="

static int
minisandbox_container_path(user_addr_t arg)
{
	struct {
		int64_t     pid;
		uint64_t    pidversion;
		user_addr_t buffer;
		user_size_t size;
		uint64_t    check_pidversion;
	} args;
	const size_t prefix_len = sizeof(SANDBOX_CONTAINER_VARIABLE) - 1;
	proc_t p;
	task_t task;
	vm_map_t map;
	vm_map_copy_t copy = NULL;
	vm_offset_t strings = 0;
	vm_size_t strings_size;
	user_addr_t user_stack;
	size_t argslen, len;
	const char *s, *end;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	p = proc_find((int)args.pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}
	if (args.check_pidversion && (uint64_t)proc_pidversion(p) != args.pidversion) {
		proc_rele(p);
		return ESRCH;
	}
	user_stack = p->user_stack;
	argslen = p->p_argslen;
	task = proc_task(p);
	if (user_stack == 0 || argslen == 0 || task == TASK_NULL) {
		proc_rele(p);
		return 0;
	}
	/* the same way as sysctl_procargsx(): the task keeps the map alive once the process is let go */
	task_reference(task);
	proc_rele(p);
	map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return 0;
	}
	strings_size = vm_map_round_page(argslen, vm_map_page_mask(map));
	if (kmem_alloc(kernel_map, &strings, strings_size, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		vm_map_deallocate(map);
		return ENOMEM;
	}
	if (vm_map_copyin(map, (vm_map_address_t)(user_stack - strings_size), strings_size, FALSE, &copy) != KERN_SUCCESS) {
		copy = NULL;
	}
	vm_map_deallocate(map);
	if (copy == NULL || vm_map_copy_overwrite(kernel_map, strings, copy, strings_size, FALSE) != KERN_SUCCESS) {
		if (copy != NULL) {
			vm_map_copy_discard(copy);
		}
		kmem_free(kernel_map, strings, strings_size);
		return 0;                       /* the process is on its way out, or has unmapped its own stack */
	}

	end = (const char *)(strings + strings_size);
	error = ENOENT;
	for (s = end - argslen; s < end; s += len + 1) {
		len = strnlen(s, end - s);
		if (len > prefix_len && s + len < end && strncmp(s, SANDBOX_CONTAINER_VARIABLE, prefix_len) == 0) {
			s += prefix_len;
			len -= prefix_len;
			error = len < args.size ? copyout(s, args.buffer, len + 1) : ENAMETOOLONG;
			break;
		}
	}
	kmem_free(kernel_map, strings, strings_size);
	return error;
}

/*
 * Which processes are sandboxed.
 *
 * Nothing is confined here, but a process can ask whether it is: sandbox_check(getpid(), NULL, 0)
 * is 1 in a sandboxed process and 0 in any other, and code relies on that. BlastDoor (the parser
 * of every incoming message, MessagesBlastDoorService and IDSBlastDoorService) refuses to run
 * otherwise: "BlastDoor/Sandbox.swift:38: Fatal error: BlastDoor cannot run unsandboxed." and a
 * SIGTRAP 50 ms after launch, so no SMS ever reached Messages while every answer was 0
 * (BlastDoor.framework, 0x1a9bf9914 in the 20F66 shared cache: getpid, __bd_sandbox_check, which
 * is sandbox_check(pid, NULL, 0), and cmp w0, #1).
 *
 * On a device Sandbox.kext applies a profile when the program is executed, the builtin profile
 * named by its seatbelt-profiles entitlement (BlastDoor's is blastdoor-messages; some 280
 * programs of the system have one), or when the process asks for one itself (sandbox_init*,
 * request 1; sandbox_apply, request 0; sandbox_init_from_pid, request 0x1e). The profile stays
 * across exec and goes to every child. That is what is recorded here, by the process's unique
 * id, which fork and spawn give anew and exec keeps; the record goes with the process.
 *
 * The part between the two host-testable markers is plain C; tools/test-sandbox.py compiles it
 * on the host.
 */
/* host-testable: begin */

/*
 * Request 2, "check": sandbox_check(), sandbox_check_by_audit_token() and sandbox_check_by_uniqueid()
 * all fill in this block (_sandbox_check_common of libsystem_sandbox 1846.120.15). An operation of
 * NULL asks whether the process is sandboxed at all; the answer is the first 64 bits of the result.
 */
struct minisandbox_check_args {
	uint64_t result;        /* user address of 40 bytes, zeroed by the caller */
	int64_t  pid;
	uint64_t operation;     /* user address of the operation's name, or 0 */
	uint64_t filter_type;
	uint64_t filter_arg;
	uint64_t flags;         /* SANDBOX_CHECK_* in the low bits, and how the process is named: */
	uint64_t id;            /* the pid version of an audit token, or the unique id */
};
#define MINISANDBOX_CHECK_BY_AUDIT_TOKEN 0x40000000ULL
#define MINISANDBOX_CHECK_BY_UNIQUEID    0x80000000ULL

/* Request 0x41, sandbox_enable_state_flag(name, audit token). */
struct minisandbox_state_flag_args {
	int64_t  pid;
	uint64_t pidversion;
	uint64_t name;          /* user address */
};

/* Request 5, __sandbox_extension_issue(): the token goes to a buffer of 0x800 bytes from malloc(). */
struct minisandbox_issue_args {
	uint64_t extension_class;       /* user address, e.g. "com.apple.app-sandbox.read" */
	uint64_t type;
	uint64_t name;                  /* user address: path, service, ...; may be 0 */
	uint64_t flags;
	uint64_t token;                 /* user address */
	int64_t  pid;
	uint64_t pidversion;
};
#define MINISANDBOX_TOKEN_SIZE 0x800

/* Request 6, sandbox_extension_consume(token): { token, strlen(token) + 1, user address of the int64 handle }. */
struct minisandbox_consume_args {
	uint64_t token;                 /* user address */
	uint64_t length;
	uint64_t handle;                /* user address; libsandbox returns it if the request gives 0, else -1 */
};

/*
 * Request 0x1c, sandbox_reference_retain_by_audit_token(token): { pid, pid version, user address of the
 * int64 reference }, which libsandbox sets to -1 first and returns as it is when the request gives 0.
 * The reference goes to sandbox_check_by_reference() (request 2 with pid 0, both of the flags below and
 * the reference as the id) and to sandbox_reference_release() (request 0x1d { reference }).
 */
struct minisandbox_reference_args {
	int64_t  pid;
	uint64_t pidversion;
	uint64_t reference;             /* user address */
};
#define MINISANDBOX_CHECK_BY_REFERENCE (MINISANDBOX_CHECK_BY_AUDIT_TOKEN | MINISANDBOX_CHECK_BY_UNIQUEID)

/*
 * Whether an executable is a third-party application's: installd puts them in
 * /private/var/containers/Bundle/Application/<UUID>/<Name>.app/ (/var is a link to /private/var).
 * On a device Sandbox.kext gives every such program the builtin profile "container".
 */
static int
minisandbox_app_path(const char *path)
{
	static const char prefix[] = "/var/containers/Bundle/Application/";
	const size_t prefix_len = sizeof(prefix) - 1;

	if (strncmp(path, "/private/", 9) == 0) {
		path += sizeof("/private") - 1;
	}
	/* something after the prefix: the UUID directory at least */
	return strncmp(path, prefix, prefix_len) == 0 && path[prefix_len] != '\0' && path[prefix_len] != '/';
}

/* 1 for n = 1, 2, 4, 8, ...: the rate of the log lines for things that happen hundreds of times */
static int
minisandbox_log_now(unsigned n)
{
	return n != 0 && (n & (n - 1)) == 0;
}

#define MINISANDBOX_PROFILE_MAX 40
#define MINISANDBOX_MAX_PROCS   1024
#define MINISANDBOX_REPORTED    0x1     /* its own sandbox_check(self) has been logged */

struct minisandbox_proc {
	uint64_t uniqueid;
	uint32_t flags;
	char     profile[MINISANDBOX_PROFILE_MAX];
};
static struct minisandbox_proc minisandbox_procs[MINISANDBOX_MAX_PROCS];
static unsigned minisandbox_nprocs;

static const char *
minisandbox_skip_space(const char *s, const char *end)
{
	while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
		s++;
	}
	return s;
}

/* Past optional white space and then the text "what", or 0 if that is not what follows. */
static int
minisandbox_expect(const char **s, const char *end, const char *what)
{
	size_t len = strlen(what);
	const char *at = minisandbox_skip_space(*s, end);

	if ((size_t)(end - at) < len || memcmp(at, what, len) != 0) {
		return 0;
	}
	*s = at + len;
	return 1;
}

/*
 * The first profile the seatbelt-profiles entitlement names, from the text of the XML
 * entitlements in a code signature: 1 and the name (cut to fit), or 0 if there is no such
 * entitlement or it is not an array that starts with a string.
 */
static int
minisandbox_seatbelt_profile(const char *xml, size_t xml_len, char *name, size_t size)
{
	static const char key[] = "<key>seatbelt-profiles</key>";
	const size_t key_len = sizeof(key) - 1;
	const char *end = xml + xml_len, *s, *e;
	size_t len;

	for (s = xml; (size_t)(end - s) >= key_len; s++) {
		if (*s == '<' && memcmp(s, key, key_len) == 0) {
			break;
		}
	}
	if ((size_t)(end - s) < key_len) {
		return 0;
	}
	s += key_len;
	if (!minisandbox_expect(&s, end, "<array>") || !minisandbox_expect(&s, end, "<string>")) {
		return 0;
	}
	for (e = s; e < end && *e != '<'; e++) {
	}
	len = (size_t)(e - s);
	if (len == 0 || !minisandbox_expect(&e, end, "</string>") || size == 0) {
		return 0;
	}
	if (len >= size) {
		len = size - 1;
	}
	memcpy(name, s, len);
	name[len] = '\0';
	return 1;
}

static struct minisandbox_proc *
minisandbox_lookup(uint64_t uniqueid)
{
	for (unsigned i = 0; i < minisandbox_nprocs; i++) {
		if (minisandbox_procs[i].uniqueid == uniqueid) {
			return &minisandbox_procs[i];
		}
	}
	return NULL;
}

/* 0: recorded; EEXIST: sandboxed already (its first profile stays); ENOSPC: the table is full. */
static int
minisandbox_record(uint64_t uniqueid, const char *profile)
{
	struct minisandbox_proc *entry;

	if (minisandbox_lookup(uniqueid) != NULL) {
		return EEXIST;
	}
	if (minisandbox_nprocs == MINISANDBOX_MAX_PROCS) {
		return ENOSPC;
	}
	entry = &minisandbox_procs[minisandbox_nprocs++];
	entry->uniqueid = uniqueid;
	entry->flags = 0;
	strlcpy(entry->profile, profile, sizeof(entry->profile));
	return 0;
}

/*
 * Like minisandbox_record(), but a profile the process already has (inherited from its parent) is
 * replaced: 0 new, EEXIST replaced, ENOSPC full. On a device an application is in "container"
 * whoever started it.
 */
static int
minisandbox_record_replace(uint64_t uniqueid, const char *profile)
{
	struct minisandbox_proc *entry = minisandbox_lookup(uniqueid);

	if (entry == NULL) {
		return minisandbox_record(uniqueid, profile);
	}
	strlcpy(entry->profile, profile, sizeof(entry->profile));
	return EEXIST;
}

static void
minisandbox_forget(uint64_t uniqueid)
{
	struct minisandbox_proc *entry = minisandbox_lookup(uniqueid);

	if (entry != NULL) {
		*entry = minisandbox_procs[--minisandbox_nprocs];
	}
}

/*
 * The text of an issued extension. On a device it is an HMAC and the fields the kext checks when
 * the extension is consumed; nothing is checked here, but the token has to be a string: the
 * library hands its caller the malloc()ed buffer as it is when the request succeeds, and the
 * caller sends it on (in an XPC message, a bookmark). It has the shape of a real one: hash,
 * flags, three numbers, the class with its length, the name.
 */
static int
minisandbox_token(char *token, size_t size, const char *extension_class, const char *name)
{
	static const char hash[] = "0000000000000000000000000000000000000000000000000000000000000000";
	int len = snprintf(token, size, "%s;00;00000000;00000000;00000000;%016llx;%s;%s", hash,
	    (unsigned long long)strlen(extension_class), extension_class, name);

	return len < 0 || (size_t)len >= size ? ENAMETOOLONG : 0;
}
/* host-testable: end */

#define SANDBOX_SYSCALL_SET_PROFILE     0       /* sandbox_apply(): a compiled profile */
#define SANDBOX_SYSCALL_SET_BUILTIN     1       /* sandbox_init*(): { name of a builtin profile, ... } */
#define SANDBOX_SYSCALL_CHECK           2
#define SANDBOX_SYSCALL_EXTENSION_ISSUE 5
#define SANDBOX_SYSCALL_EXTENSION_CONSUME 6
#define SANDBOX_SYSCALL_REFERENCE_RETAIN 0x1c
#define SANDBOX_SYSCALL_INIT_FROM_PID   0x1e    /* { pid }: the sandbox of that process */
#define SANDBOX_SYSCALL_STATE_FLAG      0x41

/*
 * Boot-arg sbx:
 *   0 = nobody sandboxed (every request but container_path gets 0, as before v1);
 *   1 = only daemons (seatbelt-profiles at exec);
 *   absent or any other value = daemons + apps (container profile).
 */
static TUNABLE(uint32_t, minisandbox_mode, "sbx", 2);
static LCK_GRP_DECLARE(minisandbox_lck_grp, "minisandbox");
static LCK_MTX_DECLARE(minisandbox_lock, &minisandbox_lck_grp);
static unsigned minisandbox_total;      /* processes recorded since boot */
static unsigned minisandbox_app_total;  /* container-sandboxed apps */
static unsigned minisandbox_unsandboxed_reports;

static void
minisandbox_mark(proc_t p, const char *profile, const char *how, bool always_log)
{
	static bool full_reported;
	unsigned total = 0;
	int error;

	lck_mtx_lock(&minisandbox_lock);
	error = minisandbox_record(proc_uniqueid(p), profile);
	if (error == 0) {
		total = ++minisandbox_total;
	}
	lck_mtx_unlock(&minisandbox_lock);
	if (error == ENOSPC && !full_reported) {
		full_reported = true;
		minisandbox_say("minisandbox: table full (%d processes): %s[%d] not recorded as sandboxed\n",
		    MINISANDBOX_MAX_PROCS, p->p_comm, proc_getpid(p));
	}
	/* by exec there are a few hundred: those are counted, at powers of two */
	if (error == 0 && (always_log || (total & (total - 1)) == 0)) {
		minisandbox_say("minisandbox: sandboxed %s[%d] by %s, profile %s (%u since boot)\n", p->p_comm,
		    proc_getpid(p), how, profile, total);
	}
}

/* Whether the process is sandboxed, and its profile; the log line for its own question once. */
static bool
minisandbox_sandboxed(proc_t p, char *profile, size_t size, bool self)
{
	struct minisandbox_proc *entry;
	bool report = false;

	lck_mtx_lock(&minisandbox_lock);
	entry = minisandbox_lookup(proc_uniqueid(p));
	if (entry != NULL) {
		strlcpy(profile, entry->profile, size);
		if (self && (entry->flags & MINISANDBOX_REPORTED) == 0) {
			entry->flags |= MINISANDBOX_REPORTED;
			report = true;
		}
	} else if (self && minisandbox_unsandboxed_reports < 16) {
		minisandbox_unsandboxed_reports++;
		report = true;
	}
	lck_mtx_unlock(&minisandbox_lock);
	if (report) {
		minisandbox_say("minisandbox: sandbox_check(self) by %s[%d]: %s%s\n", p->p_comm, proc_getpid(p),
		    entry != NULL ? "sandboxed, profile " : "not sandboxed", entry != NULL ? profile : "");
	}
	return entry != NULL;
}

/* The process named by pid and, if given, its pid version or unique id; NULL if gone. */
static proc_t
minisandbox_find(int64_t pid, uint64_t flags, uint64_t id)
{
	proc_t p = proc_find((int)pid);

	if (p != PROC_NULL &&
	    (((flags & MINISANDBOX_CHECK_BY_AUDIT_TOKEN) && (uint32_t)proc_pidversion(p) != (uint32_t)id) ||
	    ((flags & MINISANDBOX_CHECK_BY_UNIQUEID) && proc_uniqueid(p) != id))) {
		proc_rele(p);
		p = PROC_NULL;
	}
	return p;
}

static int
minisandbox_check(proc_t caller, user_addr_t arg)
{
	struct minisandbox_check_args args;
	char profile[MINISANDBOX_PROFILE_MAX];
	uint64_t answer = 1;
	proc_t target;
	bool sandboxed;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	if (args.operation != 0) {
		return 0;               /* every operation is allowed: the answer stays 0 */
	}
	if ((args.flags & MINISANDBOX_CHECK_BY_REFERENCE) == MINISANDBOX_CHECK_BY_REFERENCE) {
		/* a reference (request 0x1c) is the unique id; it may outlive the process */
		lck_mtx_lock(&minisandbox_lock);
		sandboxed = minisandbox_lookup(args.id) != NULL;
		lck_mtx_unlock(&minisandbox_lock);
		return sandboxed ? copyout(&answer, (user_addr_t)args.result, sizeof(answer)) : 0;
	}
	target = minisandbox_find(args.pid, args.flags, args.id);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	sandboxed = minisandbox_sandboxed(target, profile, sizeof(profile), target == caller);
	proc_rele(target);
	return sandboxed ? copyout(&answer, (user_addr_t)args.result, sizeof(answer)) : 0;
}

/*
 * A state flag is a bit of the process's sandbox that its profile can test; BlastDoor sets
 * "blastdoor-post-launch" once it has started (and only logs if that fails). There is no profile
 * to test it here, so it is only answered: set, if the process has a sandbox to hold it.
 */
static int
minisandbox_state_flag(proc_t caller, user_addr_t arg)
{
	struct minisandbox_state_flag_args args;
	char name[64] = "", profile[MINISANDBOX_PROFILE_MAX];
	proc_t target;
	bool sandboxed;
	size_t done;
	int error, pid;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	(void)copyinstr((user_addr_t)args.name, name, sizeof(name), &done);
	name[sizeof(name) - 1] = '\0';
	target = minisandbox_find(args.pid, MINISANDBOX_CHECK_BY_AUDIT_TOKEN, args.pidversion);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	pid = proc_getpid(target);
	sandboxed = minisandbox_sandboxed(target, profile, sizeof(profile), false);
	proc_rele(target);
	minisandbox_say("minisandbox: state flag \"%s\" of [%d] from %s[%d]: %s\n", name, pid, caller->p_comm,
	    proc_getpid(caller), sandboxed ? "set" : "refused, not sandboxed");
	return sandboxed ? 0 : EINVAL;
}

static int
minisandbox_issue(user_addr_t arg)
{
	struct minisandbox_issue_args args;
	char extension_class[128] = "";
	char *name, *token;
	size_t done;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	(void)copyinstr((user_addr_t)args.extension_class, extension_class, sizeof(extension_class), &done);
	extension_class[sizeof(extension_class) - 1] = '\0';
	name = kalloc_data(MAXPATHLEN, Z_WAITOK | Z_ZERO);
	token = kalloc_data(MINISANDBOX_TOKEN_SIZE, Z_WAITOK | Z_ZERO);
	if (name == NULL || token == NULL) {
		error = ENOMEM;
	} else {
		if (args.name != 0) {
			(void)copyinstr((user_addr_t)args.name, name, MAXPATHLEN, &done);
			name[MAXPATHLEN - 1] = '\0';
		}
		error = minisandbox_token(token, MINISANDBOX_TOKEN_SIZE, extension_class, name);
		if (error == 0) {
			error = copyout(token, (user_addr_t)args.token, strlen(token) + 1);
		}
	}
	if (name != NULL) {
		kfree_data(name, MAXPATHLEN);
	}
	if (token != NULL) {
		kfree_data(token, MINISANDBOX_TOKEN_SIZE);
	}
	return error;
}

/*
 * Request 6: the token is taken whatever it reads (nothing is enforced) and the caller gets a handle
 * of its own, from 1 up. The request used to give 0 without writing it, so every consume "worked" with
 * handle 0, which some callers keep as "nothing to release".
 */
static int
minisandbox_consume(user_addr_t arg)
{
	static uint64_t next_handle;
	struct minisandbox_consume_args args;
	int64_t handle;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	handle = (int64_t)os_atomic_inc(&next_handle, relaxed);
	return copyout(&handle, (user_addr_t)args.handle, sizeof(handle));
}

/*
 * Request 0x1c: the reference is the unique id of the process the audit token names (ESRCH if it is
 * gone). It used to give 0 without writing anything, so the caller got the -1 libsandbox starts with.
 */
static int
minisandbox_reference_retain(user_addr_t arg)
{
	struct minisandbox_reference_args args;
	uint64_t reference;
	proc_t target;
	int error;

	error = copyin(arg, &args, sizeof(args));
	if (error) {
		return error;
	}
	target = minisandbox_find(args.pid, MINISANDBOX_CHECK_BY_AUDIT_TOKEN, args.pidversion);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	reference = proc_uniqueid(target);
	proc_rele(target);
	return copyout(&reference, (user_addr_t)args.reference, sizeof(reference));
}

/* Requests 0, 1 and 0x1e: a process gives itself a sandbox. They succeed; it is recorded. */
static void
minisandbox_apply(proc_t caller, int call, user_addr_t arg)
{
	char profile[MINISANDBOX_PROFILE_MAX] = "";
	uint64_t first = 0;
	proc_t source;
	size_t done;

	if (call == SANDBOX_SYSCALL_SET_PROFILE) {
		minisandbox_mark(caller, "(compiled)", "sandbox_apply", true);
		return;
	}
	if (copyin(arg, &first, sizeof(first)) != 0) {
		return;
	}
	if (call == SANDBOX_SYSCALL_SET_BUILTIN) {
		if (copyinstr((user_addr_t)first, profile, sizeof(profile), &done) != 0) {
			strlcpy(profile, "(builtin)", sizeof(profile));
		}
		profile[sizeof(profile) - 1] = '\0';
		minisandbox_mark(caller, profile, "sandbox_init", true);
		return;
	}
	source = proc_find((int)(int64_t)first);
	if (source != PROC_NULL && minisandbox_sandboxed(source, profile, sizeof(profile), false)) {
		minisandbox_mark(caller, profile, "sandbox_init_from_pid", true);
	}
	if (source != PROC_NULL) {
		proc_rele(source);
	}
}

/* fork and posix_spawn: the child has its parent's sandbox (the caller is the parent) */
static void
minisandbox_cred_label_associate_fork(__unused kauth_cred_t cred, proc_t child)
{
	proc_t parent = current_proc();
	struct minisandbox_proc *entry;
	uint64_t parent_id, child_id;

	if (minisandbox_mode == 0 || parent == PROC_NULL || child == PROC_NULL) {
		return;
	}
	parent_id = proc_uniqueid(parent);
	child_id = proc_uniqueid(child);
	if (parent_id == child_id) {
		return;
	}
	lck_mtx_lock(&minisandbox_lock);
	entry = minisandbox_lookup(parent_id);
	if (entry != NULL && minisandbox_record(child_id, entry->profile) == 0) {
		minisandbox_total++;
	}
	lck_mtx_unlock(&minisandbox_lock);
}

/*
 * exec of a third-party application (sbx= other than 0 and 1): profile "container", replacing one
 * inherited from the parent. Logged for the 1st, 2nd, 4th, 8th... application.
 */
static void
minisandbox_mark_app(proc_t p)
{
	static bool full_reported;
	unsigned apps = 0;
	vnode_t vp;
	char *path;
	int len = MAXPATHLEN, error;
	bool app;

	vp = proc_getexecutablevnode(p);
	if (vp == NULLVP) {
		return;
	}
	path = kalloc_data(MAXPATHLEN, Z_WAITOK | Z_ZERO);
	app = path != NULL && vn_getpath(vp, path, &len) == 0 && minisandbox_app_path(path);
	vnode_put(vp);
	if (app) {
		lck_mtx_lock(&minisandbox_lock);
		error = minisandbox_record_replace(proc_uniqueid(p), "container");
		if (error == 0) {
			minisandbox_total++;
		}
		if (error != ENOSPC) {
			apps = ++minisandbox_app_total;
		}
		lck_mtx_unlock(&minisandbox_lock);
		if (error == ENOSPC && !full_reported) {
			full_reported = true;
			minisandbox_say("minisandbox: table full (%d processes): app %s[%d] not recorded as sandboxed\n",
			    MINISANDBOX_MAX_PROCS, p->p_comm, proc_getpid(p));
		}
		if (minisandbox_log_now(apps)) {
			minisandbox_say("minisandbox: sandboxed app %s[%d], profile container (%u apps since boot)\n",
			    p->p_comm, proc_getpid(p), apps);
		}
	}
	if (path != NULL) {
		kfree_data(path, MAXPATHLEN);
	}
}

/*
 * exec: the profile its seatbelt-profiles entitlement names (a sandbox it had stays), or else
 * "container" if it is a third-party application.
 */
static void
minisandbox_proc_notify_exec_complete(proc_t p)
{
	char profile[MINISANDBOX_PROFILE_MAX];
	void *blob = NULL;
	size_t length = 0;

	if (minisandbox_mode == 0) {
		return;
	}
	if (cs_entitlements_blob_get(p, &blob, &length) == 0 && blob != NULL && length > sizeof(CS_GenericBlob) &&
	    minisandbox_seatbelt_profile(((const CS_GenericBlob *)blob)->data, length - sizeof(CS_GenericBlob),
	    profile, sizeof(profile))) {
		minisandbox_mark(p, profile, "seatbelt-profiles at exec", false);
		return;
	}
	if (minisandbox_mode != 1) {
		minisandbox_mark_app(p);
	}
}

static void
minisandbox_proc_notify_exit(proc_t p)
{
	lck_mtx_lock(&minisandbox_lock);
	minisandbox_forget(proc_uniqueid(p));
	lck_mtx_unlock(&minisandbox_lock);
}

/* Which request numbers have been seen: one log line each, to learn what user space asks for. */
static uint64_t minisandbox_seen;

static int
minisandbox_policy_syscall(struct proc *p, int call, user_addr_t arg)
{
	bool answered = call == SANDBOX_SYSCALL_CONTAINER_PATH ||
	    (minisandbox_mode != 0 && (call == SANDBOX_SYSCALL_CHECK || call == SANDBOX_SYSCALL_STATE_FLAG ||
	    call == SANDBOX_SYSCALL_EXTENSION_ISSUE || call == SANDBOX_SYSCALL_EXTENSION_CONSUME ||
	    call == SANDBOX_SYSCALL_REFERENCE_RETAIN));

	if (call >= 0 && call < 64 && (minisandbox_seen & (1ULL << call)) == 0) {
		minisandbox_seen |= 1ULL << call;
		minisandbox_say("minisandbox: request %d (first seen from %s[%d]): %s\n", call, p->p_comm, proc_getpid(p),
		    answered ? "answered" : "allowed");
	}
	if (call == SANDBOX_SYSCALL_CONTAINER_PATH) {
		return minisandbox_container_path(arg);
	}
	if (minisandbox_mode == 0) {
		return 0;
	}
	switch (call) {
	case SANDBOX_SYSCALL_CHECK:
		return minisandbox_check(p, arg);
	case SANDBOX_SYSCALL_STATE_FLAG:
		return minisandbox_state_flag(p, arg);
	case SANDBOX_SYSCALL_EXTENSION_ISSUE:
		return minisandbox_issue(arg);
	case SANDBOX_SYSCALL_EXTENSION_CONSUME:
		return minisandbox_consume(arg);
	case SANDBOX_SYSCALL_REFERENCE_RETAIN:
		return minisandbox_reference_retain(arg);
	case SANDBOX_SYSCALL_SET_PROFILE:
	case SANDBOX_SYSCALL_SET_BUILTIN:
	case SANDBOX_SYSCALL_INIT_FROM_PID:
		minisandbox_apply(p, call, arg);
		return 0;
	default:
		return 0;
	}
}

/*
 * Privileges for processes that are not root. On a device the sandbox hands them out by
 * entitlement: a privilege is granted to a process whose code signature has the entitlement
 * that goes with it. The table is the same pairing: the names are the entitlements Darwin's own
 * programs carry for them (dasd "vm-pressure-level", symptomsd and aggregated
 * com.apple.private.network.statistics, mediaserverd and mDNSResponder
 * com.apple.private.network.socket-delegate -- without which NECP said "does not hold the
 * necessary entitlement to delegate network traffic" -- and so on).
 *
 * That NECP line is worth knowing for what it is NOT: it costs nothing. The macro that prints it
 * (bsd/net/necp.c, NECP_VERIFY_DELEGATION_ENTITLEMENT) only breaks out of the parameter it was
 * reading, so the delegation is dropped and the policy is matched against the caller itself; no
 * error is returned and no connection is refused. A session where trustd prints it four hundred
 * times is a session where trustd's own connections all go through. A stock iPhone prints it too.
 * It is noise, not a wall -- do not go looking for a daemon's missing network behind it.
 *
 *   PRIV_WORK_INTERVAL    is granted to every process, entitlement or not: telling the scheduler
 *                         about the work of drawing a frame. UIKit does it as soon as anything
 *                         scrolls, and takes a refusal for a programming error ("Failed to create
 *                         work interval"): SpringBoard died of it on the first swipe between
 *                         pages of the home screen.
 *
 * Whatever is refused is said once, to learn what there is. Boot-arg privs=0: only
 * PRIV_WORK_INTERVAL, as before.
 */
extern kern_return_t miniamfi_entitlement_boolean(struct proc *proc, const char *name);

static const struct {
	int priv;
	const char *entitlement;
} minisandbox_privileges[] = {
	{ PRIV_PROC_UUID_POLICY, "com.apple.private.kernel.uuid-policy" },
	{ PRIV_GLOBAL_PROC_INFO, "com.apple.private.kernel.global-proc-info" },
	{ PRIV_SYSTEM_OVERRIDE, "com.apple.private.kernel.system-override" },
	{ PRIV_HW_DEBUG_DATA, "com.apple.private.kernel.hw-debug-data" },
	{ PRIV_SELECTIVE_FORCED_IDLE, "com.apple.private.kernel.selective-forced-idle" },
	{ PRIV_DARKBOOT, "com.apple.private.kernel.darkboot" },
	{ PRIV_AUDIO_LATENCY, "com.apple.private.kernel.audio_latency" },
	{ PRIV_KTRACE_BACKGROUND, "com.apple.private.kernel.ktrace-background" },
	{ PRIV_SETPRIORITY_DARWIN_ROLE, "com.apple.private.kernel.setpriority-darwin-role" },
	{ PRIV_PACKAGE_EXTENSIONS, "com.apple.private.kernel.set-package-extensions" },
	{ PRIV_TRIM_ACTIVE_FILE, "com.apple.private.apfs.trim-active-file" },
	{ PRIV_PROC_CPUMON_OVERRIDE, "com.apple.private.kernel.override-cpumon" },
	{ PRIV_VM_PRESSURE, "vm-pressure-level" },
	{ PRIV_VM_JETSAM, "com.apple.private.kernel.jetsam" },
	{ PRIV_NET_PRIVILEGED_TRAFFIC_CLASS, "com.apple.private.network.privileged-traffic-class" },
	{ PRIV_NET_PRIVILEGED_SOCKET_DELEGATE, "com.apple.private.network.socket-delegate" },
	{ PRIV_NET_INTERFACE_CONTROL, "com.apple.private.network.interface-control" },
	{ PRIV_NET_PRIVILEGED_NETWORK_STATISTICS, "com.apple.private.network.statistics" },
	{ PRIV_NET_PRIVILEGED_NECP_POLICIES, "com.apple.private.necp.policies" },
	{ PRIV_NET_RESTRICTED_AWDL, "com.apple.private.network.awdl.restricted" },
	{ PRIV_NET_QOSMARKING_POLICY_OVERRIDE, "com.apple.private.network.qos-marking-policy-override" },
	{ PRIV_NET_RESTRICTED_MULTIPATH_EXTENDED, "com.apple.developer.networking.multipath_extended" },
	{ PRIV_NET_RESTRICTED_ROUTE_NC_READ, "com.apple.developer.networking.route_nc_read" },
	{ PRIV_NET_VALIDATED_RESOLVER, "com.apple.private.validated-resolver" },
	{ PRIV_NET_CUSTOM_PROTOCOL, "com.apple.developer.networking.custom-protocol" },
	{ PRIV_NET_PRIVILEGED_NECP_DROP_ALL_BYPASS, "com.apple.private.necp.drop_all_bypass" },
	{ PRIV_NET_PRIVILEGED_IPSEC_WAKE_PACKET, "com.apple.private.network.ipsec-wake-packet" },
	{ PRIV_NETINET_RESERVEDPORT, "com.apple.private.network.reserved-port" },
	{ PRIV_NETINET_TCP_KA_OFFLOAD, "com.apple.developer.networking.tcp_ka_offload" },
	{ PRIV_SKYWALK_REGISTER_USER_PIPE, "com.apple.private.skywalk.register-user-pipe" },
	{ PRIV_SKYWALK_REGISTER_KERNEL_PIPE, "com.apple.private.skywalk.register-kernel-pipe" },
	{ PRIV_SKYWALK_REGISTER_NET_IF, "com.apple.private.skywalk.register-net-if" },
	{ PRIV_SKYWALK_REGISTER_FLOW_SWITCH, "com.apple.private.skywalk.register-flow-switch" },
	{ PRIV_SKYWALK_OBSERVE_ALL, "com.apple.private.skywalk.observe-all" },
	{ PRIV_SKYWALK_OBSERVE_STATS, "com.apple.private.skywalk.observe-stats" },
	{ PRIV_VFS_OPEN_BY_ID, "com.apple.private.vfs.open-by-id" },
	{ PRIV_VFS_MOVE_DATA_EXTENTS, "com.apple.private.vfs.move-data-extents" },
	{ PRIV_VFS_SNAPSHOT, "com.apple.private.vfs.snapshot" },
	{ PRIV_VFS_DATALESS_RESOLVER, "com.apple.private.vfs.dataless-resolver" },
	{ PRIV_VFS_SETSIZE, "com.apple.private.vfs.setsize" },
};
static TUNABLE(uint32_t, minisandbox_by_entitlement, "privs", 1);

static int
minisandbox_priv_grant(kauth_cred_t cred, int priv)
{
	static int seen[32];
	static unsigned seen_count;
	proc_t proc = current_proc();

	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	/* (the entitlements are the current process's: only for a question about its own credential) */
	for (unsigned i = 0; minisandbox_by_entitlement && cred == kauth_cred_get() &&
	    i < sizeof(minisandbox_privileges) / sizeof(minisandbox_privileges[0]); i++) {
		if (minisandbox_privileges[i].priv == priv) {
			if (miniamfi_entitlement_boolean(proc, minisandbox_privileges[i].entitlement) == KERN_SUCCESS) {
				return 0;
			}
			break;
		}
	}
	for (unsigned i = 0; i < seen_count; i++) {
		if (seen[i] == priv) {
			return EPERM;
		}
	}
	if (seen_count < sizeof(seen) / sizeof(seen[0])) {
		seen[seen_count++] = priv;
		minisandbox_say("minisandbox: privilege %d refused to %s[%d] (first time)\n", priv, proc->p_comm,
		    proc_getpid(proc));
	}
	return EPERM;
}

static const struct mac_policy_ops minisandbox_ops = {
	.mpo_cred_label_associate_fork = minisandbox_cred_label_associate_fork,
	.mpo_policy_syscall = minisandbox_policy_syscall,
	.mpo_priv_grant = minisandbox_priv_grant,
	.mpo_proc_notify_exec_complete = minisandbox_proc_notify_exec_complete,
	.mpo_proc_notify_exit = minisandbox_proc_notify_exit,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) minisandbox_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "Seatbelt sandbox policy (stand-in)",
	.mpc_ops = &minisandbox_ops,
};
SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) minisandbox_handle;

/* Called from kernel_bootstrap_thread() once the MAC framework is up. */
void
minimac_policy_init(void)
{
	mac_policy_register(&miniamfi_conf, &miniamfi_handle, NULL);
	mac_policy_register(&minisandbox_conf, &minisandbox_handle, NULL);
}
