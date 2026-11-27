/* Exercise the same NV12 slice boundaries as Camera's preview workers. */
#include "camera_isp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
	RAW_WIDTH = 1280, RAW_HEIGHT = 720, RAW_STRIDE = 1600,
	PREVIEW_WIDTH = 1504, PREVIEW_HEIGHT = 1128, PREVIEW_STRIDE = 1536,
	FIRST_ROW = 140, END_ROW = 282, SENTINEL = 0xa5
};

static int
check_chroma(const uint8_t *plane)
{
	for (unsigned row = 0; row < PREVIEW_HEIGHT / 2; row++) {
		for (unsigned col = 0; col < PREVIEW_STRIDE; col++) {
			bool touched = row >= FIRST_ROW / 2 && row < END_ROW / 2 && col < PREVIEW_WIDTH;

			if (!touched && plane[row * PREVIEW_STRIDE + col] != SENTINEL) {
				fprintf(stderr, "NV12 preview modified chroma outside its slice at row %u, byte %u\n", row, col);
				return 1;
			}
		}
	}
	size_t center = (size_t)(FIRST_ROW + END_ROW) / 4 * PREVIEW_STRIDE + PREVIEW_WIDTH / 2;
	if (plane[center] == SENTINEL) {
		fprintf(stderr, "NV12 preview did not render the test slice\n");
		return 1;
	}
	return 0;
}

int
main(void)
{
	struct isp isp;
	const struct hakios_camera_frame frame = {
		.width = RAW_WIDTH, .height = RAW_HEIGHT, .stride = RAW_STRIDE, .black = 64,
		.gain = 512, .gain_unity = 32
	};
	size_t luma_bytes = (size_t)PREVIEW_STRIDE * PREVIEW_HEIGHT;
	uint8_t *raw = malloc((size_t)RAW_STRIDE * RAW_HEIGHT);
	uint8_t *output = malloc(luma_bytes + luma_bytes / 2);
	if (raw == NULL || output == NULL) {
		free(raw);
		free(output);
		return 1;
	}

	/* Neutral RAW10 input yields centered chroma, distinguishable from the sentinel. */
	memset(raw, 0x50, (size_t)RAW_STRIDE * RAW_HEIGHT);
	memset(output, SENTINEL, luma_bytes + luma_bytes / 2);
	isp_init(&isp);
	isp_render_nv12(&isp, &frame, raw, output, PREVIEW_STRIDE, output + luma_bytes, PREVIEW_STRIDE,
	    PREVIEW_WIDTH, PREVIEW_HEIGHT, FIRST_ROW, END_ROW, false);
	int result = check_chroma(output + luma_bytes);
	free(output);
	free(raw);
	if (result == 0) {
		puts("NV12 preview keeps chroma inside the rendered rows and preserves row padding");
	}
	return result;
}
