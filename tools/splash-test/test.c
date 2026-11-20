#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../mtkloader/mtkdbg.h"
#include "../../mtkloader/splash.h"

#define BLACK 0xff000000u
#define TEAL  0xff48dbcdu
#define WHITE 0xffe7f3f6u
#define GUARD 0xdeadbeefu
#define STRIDE (LK_FB_PITCH / sizeof(uint32_t))

static uint32_t buffer[2 + PANEL_H * STRIDE];
static uint32_t calls;

static void checked_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
	/* Make sure the artwork does not depend on the underlying clip. */
	assert(w && h && w <= fbc.w && h <= fbc.h);
	assert(x <= fbc.w - w && y <= fbc.h - h);
	assert(w == h && w <= 10);
	assert(color == TEAL || color == WHITE);
	++calls;
	fb_rect(x, y, w, h, color);
}

static void check(uint32_t width, uint32_t height, uint32_t scale)
{
	for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); ++i) {
		buffer[i] = GUARD;
	}
	fbcon_init((uintptr_t)(buffer + 1), LK_FB_PITCH, 0, 1);
	fbc.w = width;
	fbc.h = height;
	fb_rect(0, 0, width, height, BLACK);
	calls = 0;
	hakios_splash_draw(width, height, checked_rect);
	assert(calls == (scale ? 213u : 0u));
	assert(buffer[0] == GUARD && buffer[1 + PANEL_H * STRIDE] == GUARD);

	uint32_t teal = 0, white = 0;
	uint32_t x0 = scale ? (width - 35 * scale) / 2 : 0;
	uint32_t y0 = scale ? (height - 24 * scale) / 2 : 0;
	for (uint32_t y = 0; y < PANEL_H; ++y) {
		for (uint32_t x = 0; x < STRIDE; ++x) {
			uint32_t color = buffer[1 + y * STRIDE + x];
			if (x >= width || y >= height) {
				assert(color == GUARD);
			} else if (color == TEAL) {
				assert(x >= x0 + 11 * scale && x < x0 + 24 * scale);
				assert(y >= y0 && y < y0 + 13 * scale);
				++teal;
			} else if (color == WHITE) {
				assert(x >= x0 && x < x0 + 35 * scale);
				assert(y >= y0 + 17 * scale && y < y0 + 24 * scale);
				++white;
			} else {
				assert(color == BLACK);
			}
		}
	}
	assert(teal == 69 * scale * scale && white == 144 * scale * scale);
	if (scale) {
		assert(buffer[1 + y0 * STRIDE + x0 + 14 * scale] == TEAL);
		assert(buffer[1 + (y0 + 17 * scale) * STRIDE + x0] == WHITE);
	}
}

int main(void)
{
	check(PANEL_W, PANEL_H, 10);
	check(173, 149, 4);
	check(350, 49, 2);
	check(35, 24, 1);
	check(34, 24, 0);
	check(35, 23, 0);
	check(0, 0, 0);
	puts("HakiOS splash: 7 framebuffer cases passed");
	return 0;
}
