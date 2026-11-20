#ifndef HAKIOS_SPLASH_H
#define HAKIOS_SPLASH_H

#include <stdint.h>

/* Original HakiOS artwork: a split H monogram in a chamfered frame and a
 * hand-drawn block wordmark. Each marked cell is an opaque square. */
static inline void hakios_splash_draw(uint32_t width, uint32_t height,
    void (*rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color))
{
	static const char mark[][14] = {
		"...#######...",
		"..#.......#..",
		".#.........#.",
		"#..##...##..#",
		"#..##...##..#",
		"#..##..###..#",
		"#..#######..#",
		"#..###..##..#",
		"#..##...##..#",
		"#..##...##..#",
		".#.........#.",
		"..#.......#..",
		"...#######...",
	};
	static const char word[][36] = {
		"##.## .###. ##.## ##### .###. .####",
		"##.## ##.## ##.## ..#.. ##.## ##...",
		"##.## ##.## ####. ..#.. ##.## ##...",
		"##### ##### ###.. ..#.. ##.## .###.",
		"##.## ##.## ####. ..#.. ##.## ...##",
		"##.## ##.## ##.## ..#.. ##.## ...##",
		"##.## ##.## ##.## ##### .###. ####.",
	};
	enum { MARK_W = 13, MARK_H = 13, WORD_W = 35, WORD_H = 7, GAP = 4,
	    SPLASH_H = MARK_H + GAP + WORD_H };
	uint32_t scale = width / WORD_W;
	if (scale > height / SPLASH_H) {
		scale = height / SPLASH_H;
	}
	if (scale > 10) {
		scale = 10;
	}
	if (!scale) {
		return;
	}

	uint32_t top = (height - SPLASH_H * scale) / 2;
	uint32_t mark_x = (width - MARK_W * scale) / 2;
	uint32_t word_x = (width - WORD_W * scale) / 2;
	for (uint32_t y = 0; y < MARK_H; y++) {
		for (uint32_t x = 0; x < MARK_W; x++) {
			if (mark[y][x] == '#') {
				rect(mark_x + x * scale, top + y * scale, scale, scale, 0xff48dbcdu);
			}
		}
	}
	for (uint32_t y = 0; y < WORD_H; y++) {
		for (uint32_t x = 0; x < WORD_W; x++) {
			if (word[y][x] == '#') {
				rect(word_x + x * scale, top + (MARK_H + GAP + y) * scale,
				    scale, scale, 0xffe7f3f6u);
			}
		}
	}
}

#endif /* HAKIOS_SPLASH_H */
