/* Start OpenSSH on thyme's Wi-Fi address for an explicitly built development scene. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hdb_network.h"
#define HDB_SSHD "/var/jb/usr/sbin/sshd"
#define PREFIX_PW "/var/jb/usr/sbin/pw"

static bool
run_pw(char *const argv[], bool quiet)
{
	pid_t pid = fork();

	if (pid == 0) {
		if (quiet) {
			int fd = open("/dev/null", O_WRONLY);

			if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
				_exit(127);
			}
			close(fd);
		}
		execv(PREFIX_PW, argv);
		_exit(127);
	}
	if (pid < 0) {
		return false;
	}
	int status;
	pid_t done;

	do {
		done = waitpid(pid, &status, 0);
	} while (done < 0 && errno == EINTR);
	return done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Procursus names UID 75 _sshd. Its OpenSSH binary looks up sshd. */
static bool
prepare_privsep(void)
{
	char *check[] = { PREFIX_PW, "usershow", "sshd", NULL };
	char *rename[] = { PREFIX_PW, "usermod", "_sshd", "-l", "sshd", NULL };
	struct statfs fs;

	if (run_pw(check, true)) {
		return true;
	}
	if (statfs("/var/jb/etc", &fs) != 0 || strcmp(fs.f_fstypename, "ramfs") != 0) {
		return false;
	}
	return run_pw(rename, false) && run_pw(check, true);
}

int
main(void)
{
	struct in_addr peer;
	char peer_text[INET_ADDRSTRLEN + 2], local[INET_ADDRSTRLEN];
	if (!hdb_read_peer(&peer, peer_text, sizeof(peer_text)) ||
	    access(HDB_DIR "/host_key", R_OK) != 0 || access(HDB_DIR "/authorized_keys", R_OK) != 0 ||
	    access(HDB_SSHD, X_OK) != 0) {
		fprintf(stderr, "hdb: development credentials or OpenSSH are missing\n");
		return 1;
	}
	for (unsigned seconds = 0; seconds < 120; seconds++) {
		if (hdb_wifi_address(peer, local, sizeof(local))) {
			char listen[sizeof("ListenAddress=") + INET_ADDRSTRLEN];
			char allow[sizeof("AllowUsers=root@") + INET_ADDRSTRLEN];

			if (!prepare_privsep()) {
				fprintf(stderr, "hdb: cannot prepare the SSH privilege separation account in RAM\n");
				return 1;
			}
			snprintf(listen, sizeof(listen), "ListenAddress=%s", local);
			snprintf(allow, sizeof(allow), "AllowUsers=root@%s", peer_text);
			fprintf(stderr, "hdb: SSH on %s:2222\n", local);
			execl(HDB_SSHD, HDB_SSHD, "-D", "-e", "-f", HDB_DIR "/sshd_config",
			    "-o", listen, "-o", allow, (char *)NULL);
			perror("hdb: sshd");
			return 1;
		}
		sleep(1);
	}
	fprintf(stderr, "hdb: no private Wi-Fi address on the authorized peer's LAN\n");
	return 1;
}
