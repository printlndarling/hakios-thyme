/* Read and write an explicitly authenticated AFC development directory. */
#include "afc-ram.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define AFC_FILE_LIMIT (16u * 1024u * 1024u)
#define AFC_WRITE_BUDGET (32u * 1024u * 1024u)
#define AFC_REPLY_MAX (16u * 1024u)
#define AFC_PATH_MAX 1024u

enum {
	AFC_STATUS = 1, AFC_DATA = 2, AFC_READ_DIR = 3, AFC_REMOVE = 8,
	AFC_MKDIR = 9, AFC_STAT = 10, AFC_DEVICE_INFO = 11, AFC_OPEN = 13,
	AFC_OPEN_RESULT = 14, AFC_READ = 15, AFC_WRITE = 16, AFC_SEEK = 17,
	AFC_TELL = 18, AFC_TELL_RESULT = 19, AFC_CLOSE = 20,
};

enum {
	AFC_OK = 0, AFC_INVALID = 7, AFC_NOT_FOUND = 8, AFC_IS_DIR = 9,
	AFC_PERMISSION = 10, AFC_LIMIT = 13, AFC_UNSUPPORTED = 15,
	AFC_EXISTS = 16, AFC_BUSY = 17, AFC_NO_SPACE = 18, AFC_IO = 20,
	AFC_NOT_EMPTY = 33,
};

static const char magic[] = "CFA6LPAA";

static uint64_t
read64(const uint8_t *data)
{
	uint64_t number = 0;
	for (unsigned i = 0; i < 8; i++) number |= (uint64_t)data[i] << (i * 8);
	return number;
}

static void
write64(uint8_t *data, uint64_t number)
{
	for (unsigned i = 0; i < 8; i++) data[i] = (uint8_t)(number >> (i * 8));
}

size_t
afc_request_size(const uint8_t *header)
{
	uint64_t entire = read64(header + 8), first = read64(header + 16);
	if (memcmp(header, magic, 8) != 0 || first < AFC_HEADER_SIZE ||
	    first > entire || entire > AFC_REQUEST_MAX) {
		return 0;
	}
	return (size_t)entire;
}

static bool
on_ramfs(int fd)
{
	struct statfs fs;
	return fstatfs(fd, &fs) == 0 && strcmp(fs.f_fstypename, "ramfs") == 0;
}

void
afc_ram_close(struct afc_ram *afc)
{
	for (size_t i = 0; i < sizeof(afc->files) / sizeof(afc->files[0]); i++) {
		if (afc->files[i].id) {
			close(afc->files[i].fd);
			afc->files[i].id = 0;
		}
	}
	if (afc->root >= 0) {
		close(afc->root);
		afc->root = -1;
	}
}

bool
afc_ram_open(struct afc_ram *afc)
{
	afc_ram_close(afc);
	int parent = open("/private/var/tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent < 0) return false;
	int root = -1;
	if (on_ramfs(parent) && (mkdirat(parent, "hakios-afc", 0700) == 0 || errno == EEXIST)) {
		root = openat(parent, "hakios-afc", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	}
	close(parent);
	if (root < 0) return false;
	struct stat st;
	if (!on_ramfs(root) || fstat(root, &st) != 0 || st.st_uid != 0 || (st.st_mode & 077) != 0) {
		close(root);
		return false;
	}
	afc->root = root;
	return true;
}

static int
afc_error(int error)
{
	switch (error) {
	case EINVAL: case ENAMETOOLONG: return AFC_INVALID;
	case ENOENT: case ENOTDIR: return AFC_NOT_FOUND;
	case EISDIR: return AFC_IS_DIR;
	case EACCES: case EPERM: case ELOOP: return AFC_PERMISSION;
	case EEXIST: return AFC_EXISTS;
	case EBUSY: return AFC_BUSY;
	case ENOSPC: return AFC_NO_SPACE;
	case EFBIG: return AFC_LIMIT;
	case ENOTEMPTY: return AFC_NOT_EMPTY;
	default: return AFC_IO;
	}
}

static bool
valid_component(const char *name)
{
	return *name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* Confine lookup to already-open directories and never follow symlinks. */
static int
open_parent(int root, const uint8_t *payload, size_t length, char *name, bool allow_root)
{
	if (!length || length > AFC_PATH_MAX || memchr(payload, '\0', length) != payload + length - 1) {
		errno = EINVAL;
		return -1;
	}
	memcpy(name, payload, length);
	char *component = name + (name[0] == '/');
	int current = openat(root, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (current < 0) return -1;
	if (!*component && allow_root) {
		memcpy(name, ".", 2);
		return current;
	}
	for (;;) {
		char *separator = strchr(component, '/');
		if (separator) *separator = '\0';
		if (!valid_component(component)) {
			close(current);
			errno = EINVAL;
			return -1;
		}
		if (!separator) {
			memmove(name, component, strlen(component) + 1);
			return current;
		}
		int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		int failure = errno;
		close(current);
		if (next < 0) {
			errno = failure;
			return -1;
		}
		if (!on_ramfs(next)) {
			close(next);
			errno = EPERM;
			return -1;
		}
		current = next;
		component = separator + 1;
	}
}

static struct afc_handle *
get_handle(struct afc_ram *afc, uint64_t id)
{
	if (id == 0) return NULL;
	for (size_t i = 0; i < sizeof(afc->files) / sizeof(afc->files[0]); i++) {
		if (afc->files[i].id == id) return &afc->files[i];
	}
	return NULL;
}

static int
open_file(struct afc_ram *afc, const uint8_t *data, size_t length, uint64_t *result)
{
	if (length <= 8) return AFC_INVALID;
	const int flags[] = {O_RDONLY, O_RDWR | O_CREAT, O_WRONLY | O_CREAT,
	    O_RDWR | O_CREAT, O_WRONLY | O_APPEND | O_CREAT, O_RDWR | O_APPEND | O_CREAT};
	uint64_t mode = read64(data);
	if (mode < 1 || mode > sizeof(flags) / sizeof(flags[0])) return AFC_INVALID;
	struct afc_handle *slot = NULL;
	for (size_t i = 0; i < sizeof(afc->files) / sizeof(afc->files[0]); i++) {
		if (!afc->files[i].id) {
			slot = &afc->files[i];
			break;
		}
	}
	if (!slot) return AFC_BUSY;
	char name[AFC_PATH_MAX];
	int parent = open_parent(afc->root, data + 8, length - 8, name, false);
	if (parent < 0) return afc_error(errno);
	int fd = openat(parent, name, flags[mode - 1] | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
	int failure = errno;
	close(parent);
	if (fd < 0) return afc_error(failure);
	struct stat st;
	int status = AFC_OK;
	if (fstat(fd, &st) != 0) {
		status = afc_error(errno);
	} else if (S_ISDIR(st.st_mode)) {
		status = AFC_IS_DIR;
	} else if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != 0 || !on_ramfs(fd)) {
		status = AFC_PERMISSION;
	} else if ((mode == 3 || mode == 4) && ftruncate(fd, 0) != 0) {
		status = afc_error(errno);
	}
	if (status != AFC_OK) {
		close(fd);
		return status;
	}
	slot->fd = fd;
	slot->id = ++afc->next_handle;
	*result = slot->id;
	return AFC_OK;
}

static int
path_command(struct afc_ram *afc, uint64_t operation, const uint8_t *data, size_t length,
    uint8_t *output, size_t capacity, size_t *written)
{
	char name[AFC_PATH_MAX];
	bool query = operation == AFC_READ_DIR || operation == AFC_STAT;
	int parent = open_parent(afc->root, data, length, name, query);
	if (parent < 0) return afc_error(errno);
	int status = AFC_OK;
	if (operation == AFC_MKDIR) {
		if (mkdirat(parent, name, 0700) != 0) status = afc_error(errno);
	} else if (operation == AFC_READ_DIR) {
		int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			status = afc_error(errno);
		} else if (!on_ramfs(fd)) {
			close(fd);
			status = AFC_PERMISSION;
		} else {
			DIR *directory = fdopendir(fd);
			if (!directory) {
				status = afc_error(errno);
				close(fd);
			} else {
				for (;;) {
					errno = 0;
					struct dirent *entry = readdir(directory);
					if (!entry) {
						if (errno) status = afc_error(errno);
						break;
					}
					size_t size = strlen(entry->d_name) + 1;
					if (size > capacity - *written) {
						status = AFC_LIMIT;
						break;
					}
					memcpy(output + *written, entry->d_name, size);
					*written += size;
				}
				closedir(directory);
			}
		}
	} else {
		struct stat st;
		if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
			status = afc_error(errno);
		} else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
			status = AFC_PERMISSION;
		} else if (operation == AFC_REMOVE) {
			if (unlinkat(parent, name, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0) != 0) status = afc_error(errno);
		} else {
			int count = snprintf((char *)output, capacity,
			    "st_ifmt%c%s%cst_size%c%jd%cst_nlink%c%ju%cst_mtime%c%" PRIu64,
			    0, S_ISDIR(st.st_mode) ? "S_IFDIR" : "S_IFREG", 0, 0, (intmax_t)st.st_size, 0,
			    0, (uintmax_t)st.st_nlink, 0, 0, (uint64_t)st.st_mtime * UINT64_C(1000000000));
			if (count < 0 || (size_t)count >= capacity) status = AFC_LIMIT;
			else *written = (size_t)count + 1;
		}
	}
	close(parent);
	return status;
}

static int
file_command(struct afc_ram *afc, uint64_t operation, const uint8_t *data, size_t length,
    uint8_t *output, size_t capacity, size_t *written)
{
	if (length < 8) return AFC_INVALID;
	struct afc_handle *handle = get_handle(afc, read64(data));
	if (!handle) return AFC_INVALID;
	int fd = handle->fd;
	switch (operation) {
	case AFC_CLOSE:
		if (length != 8) return AFC_INVALID;
		handle->id = 0;
		return close(fd) == 0 ? AFC_OK : afc_error(errno);
	case AFC_READ: {
		if (length != 16) return AFC_INVALID;
		uint64_t wanted = read64(data + 8);
		ssize_t n;
		do {
			n = read(fd, output, wanted < capacity ? (size_t)wanted : capacity);
		} while (n < 0 && errno == EINTR);
		if (n < 0) return afc_error(errno);
		*written = (size_t)n;
		return AFC_OK;
	}
	case AFC_WRITE: {
		struct stat st;
		off_t offset = lseek(fd, 0, SEEK_CUR);
		if (offset < 0 || fstat(fd, &st) != 0) return afc_error(errno);
		/* Check both the cursor and EOF, including descriptors opened for append. */
		uint64_t end = (uint64_t)(offset > st.st_size ? offset : st.st_size);
		size_t bytes = length - 8;
		if (end > AFC_FILE_LIMIT || bytes > AFC_FILE_LIMIT - end ||
		    bytes > AFC_WRITE_BUDGET - afc->bytes_written) return AFC_NO_SPACE;
		const uint8_t *start = data + 8;
		while (bytes) {
			ssize_t n = write(fd, start, bytes);
			if (n < 0 && errno == EINTR) continue;
			if (n <= 0) return n < 0 ? afc_error(errno) : AFC_IO;
			bytes -= (size_t)n;
			start += n;
			afc->bytes_written += (size_t)n;
		}
		return AFC_OK;
	}
	case AFC_SEEK: {
		if (length != 24) return AFC_INVALID;
		uint64_t origin = read64(data + 8);
		int64_t offset = (int64_t)read64(data + 16);
		if (origin > 2 || offset > AFC_FILE_LIMIT || offset < -(int64_t)AFC_FILE_LIMIT) return AFC_INVALID;
		const int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
		off_t before = lseek(fd, 0, SEEK_CUR);
		if (before < 0) return afc_error(errno);
		off_t after = lseek(fd, offset, whence[origin]);
		if (after < 0) return afc_error(errno);
		if (after > AFC_FILE_LIMIT) {
			lseek(fd, before, SEEK_SET);
			return AFC_LIMIT;
		}
		return AFC_OK;
	}
	case AFC_TELL: {
		if (length != 8) return AFC_INVALID;
		off_t offset = lseek(fd, 0, SEEK_CUR);
		if (offset < 0) return afc_error(errno);
		write64(output, (uint64_t)offset);
		*written = 8;
		return AFC_OK;
	}
	default:
		return AFC_UNSUPPORTED;
	}
}

size_t
afc_ram_reply(struct afc_ram *afc, const uint8_t *request, size_t length, uint8_t *output, size_t capacity)
{
	if (afc->root < 0 || length < AFC_HEADER_SIZE || length != afc_request_size(request) ||
	    capacity < AFC_HEADER_SIZE + 8) return 0;
	if (capacity > AFC_REPLY_MAX) capacity = AFC_REPLY_MAX;
	uint64_t operation = read64(request + 32);
	const uint8_t *data = request + AFC_HEADER_SIZE;
	size_t size = length - AFC_HEADER_SIZE, written = 0;
	uint8_t *body = output + AFC_HEADER_SIZE;
	size_t available = capacity - AFC_HEADER_SIZE;
	uint64_t reply = AFC_STATUS;
	int status = AFC_INVALID;
	if (operation == AFC_WRITE ? read64(request + 16) == AFC_HEADER_SIZE + 8 :
	    read64(request + 16) == length) {
		switch (operation) {
		case AFC_DEVICE_INFO: {
			if (size != 0) break;
			struct statfs fs;
			if (fstatfs(afc->root, &fs) != 0) {
				status = afc_error(errno);
				break;
			}
			int n = snprintf((char *)body, available, "Model%cXiaomi Mi 10S%cFSBlockSize%c%u%c"
			    "FSTotalBytes%c%u%cFSFreeBytes%c%" PRIu64, 0, 0, 0, fs.f_bsize, 0, 0,
			    AFC_WRITE_BUDGET, 0, 0, AFC_WRITE_BUDGET - afc->bytes_written);
			status = n >= 0 && (size_t)n < available ? AFC_OK : AFC_LIMIT;
			if (status == AFC_OK) written = (size_t)n + 1;
			reply = AFC_DATA;
			break;
		}
		case AFC_READ_DIR: case AFC_STAT: case AFC_MKDIR: case AFC_REMOVE:
			status = path_command(afc, operation, data, size, body, available, &written);
			if (operation == AFC_READ_DIR || operation == AFC_STAT) reply = AFC_DATA;
			break;
		case AFC_OPEN: {
			uint64_t handle;
			status = open_file(afc, data, size, &handle);
			if (status == AFC_OK) {
				write64(body, handle);
				written = 8;
				reply = AFC_OPEN_RESULT;
			}
			break;
		}
		case AFC_CLOSE: case AFC_READ: case AFC_WRITE: case AFC_SEEK: case AFC_TELL:
			status = file_command(afc, operation, data, size, body, available, &written);
			if (operation == AFC_READ) reply = AFC_DATA;
			if (operation == AFC_TELL) reply = AFC_TELL_RESULT;
			break;
		default:
			status = AFC_UNSUPPORTED;
			break;
		}
	}
	if (status != AFC_OK || reply == AFC_STATUS) {
		write64(body, (uint64_t)status);
		written = 8;
		reply = AFC_STATUS;
	}
	memcpy(output, magic, 8);
	write64(output + 8, AFC_HEADER_SIZE + written);
	write64(output + 16, AFC_HEADER_SIZE + written);
	write64(output + 24, read64(request + 24));
	write64(output + 32, reply);
	return AFC_HEADER_SIZE + written;
}
