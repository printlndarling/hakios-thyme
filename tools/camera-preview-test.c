/* Exercise Bayer order and the NV12 slice boundaries of Camera's preview workers. */
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

/* Each 2x2 source cell has distinct red and blue and two different green sites. */
static int
check_bayer(struct isp *isp)
{
	struct hakios_camera_frame frame = { .width = 4, .height = 4, .stride = 5, .black = 64 };
	uint8_t raw[20], bgra[8 * 8 * 4];
	const uint16_t red = 112, green[2] = { 160, 224 }, blue = 312;
	const uint8_t expect[] = { isp->gamma[blue * 4], isp->gamma[(green[0] + green[1]) * 2], isp->gamma[red * 4], 255 };

	for (unsigned bayer = 0; bayer < 4; bayer++) {
		uint16_t cell[4];
		unsigned g = 0;

		for (unsigned i = 0; i < 4; i++) {
			cell[i] = 64 + (i == bayer ? red : i == 3 - bayer ? blue : green[g++]);
		}
		for (unsigned y = 0; y < 4; y++) {
			uint8_t *row = raw + y * frame.stride;

			row[4] = 0;
			for (unsigned x = 0; x < 4; x++) {
				uint16_t value = cell[(y & 1) * 2 + (x & 1)];

				row[x] = (uint8_t)(value >> 2);
				row[4] |= (value & 3) << (x * 2);
			}
		}
		frame.bayer = bayer;
		isp_render_bgra(isp, &frame, raw, bgra, 8 * 4, 8, 8, 0, 8);
		for (size_t i = 0; i < sizeof(bgra); i += 4) {
			if (memcmp(bgra + i, expect, sizeof(expect)) != 0) {
				fprintf(stderr, "Bayer order %u produced wrong BGRA for pixel %zu\n", bayer, i / 4);
				return 1;
			}
		}
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
	if (result == 0 && check_bayer(&isp) == 0) {
		puts("Preview preserves RGB ordering and averages both green sites in all four Bayer orders");
		return 0;
	}
	return 1;
}
