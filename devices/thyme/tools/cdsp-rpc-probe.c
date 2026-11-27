/* A freestanding FastRPC loader diagnostic for the thyme CDSP. */
#include <stddef.h>
#include <stdint.h>

enum { PROBE_BYTES = 128 };

typedef union {
	struct {
		void *data;
		size_t size;
	} buffer;
	uint64_t handle;
} remote_arg;

#ifdef __hexagon__
_Static_assert(sizeof(remote_arg) == 8, "FastRPC DSP argument stride");
#endif

/* Method 0 takes two 128-byte inputs and returns one 128-byte output. */
int
hakios_cdsp_probe_skel_invoke(uint32_t scalars, remote_arg *args)
{
	if (scalars != 0x00020100 || !args) {
		return (int32_t)0x8000040e; /* DSP AEE_EBADPARM */
	}
	for (unsigned i = 0; i < 3; i++) {
		if (!args[i].buffer.data || args[i].buffer.size != PROBE_BYTES) {
			return (int32_t)0x8000040e;
		}
	}

	const uint8_t *left = args[0].buffer.data;
	const uint8_t *right = args[1].buffer.data;
	uint8_t *out = args[2].buffer.data;
	for (unsigned i = 0; i < PROBE_BYTES; i++) {
		out[i] = (uint8_t)(left[i] + right[i]);
	}
	return 0;
}
