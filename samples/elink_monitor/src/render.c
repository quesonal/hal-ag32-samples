/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/kernel.h>
#include <string.h>
#include "render.h"
#include "font5x5.h"

/* Canvas dimensions match the elink sample's frame memory layout: 176 pixels
 * per data-direction line (22 bytes, MSB = leftmost), 264 scan-direction
 * lines. Both axes are 0-based, clipped implicitly at the edges. */
#define CANVAS_W  176U
#define CANVAS_H  264U
#define LINE_BYTES  22U   /* (176 + 7) / 8 */

static inline uint8_t *fb_at(uint8_t *fb, uint16_t x, uint16_t y)
{
	return &fb[(size_t)y * LINE_BYTES + (x >> 3)];
}

static inline uint8_t fb_mask(uint16_t x)
{
	return (uint8_t)(0x80U >> (x & 0x07U));
}

void render_pixel(uint8_t *fb, uint16_t x, uint16_t y, bool on)
{
	uint8_t *p;
	uint8_t m;

	if (x >= CANVAS_W || y >= CANVAS_H) {
		return;
	}
	p = fb_at(fb, x, y);
	m = fb_mask(x);
	if (on) {
		*p |= m;
	} else {
		*p &= (uint8_t)~m;
	}
}

void render_fill(uint8_t *fb)
{
	memset(fb, 0xFF, CANVAS_W * (CANVAS_H / 8));
}

void render_clear(uint8_t *fb)
{
	memset(fb, 0x00, CANVAS_W * (CANVAS_H / 8));
}

void render_hline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w)
{
	for (uint16_t i = 0; i < w; i++) {
		render_pixel(fb, (uint16_t)(x + i), y, true);
	}
}

void render_vline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t h)
{
	for (uint16_t i = 0; i < h; i++) {
		render_pixel(fb, x, (uint16_t)(y + i), true);
	}
}

void render_rect(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		 bool filled)
{
	uint16_t i;

	if (w == 0 || h == 0) {
		return;
	}
	if (filled) {
		for (i = 0; i < h; i++) {
			render_hline(fb, x, (uint16_t)(y + i), w);
		}
		return;
	}
	render_hline(fb, x, y, w);
	render_hline(fb, x, (uint16_t)(y + h - 1U), w);
	render_vline(fb, x, y, h);
	render_vline(fb, (uint16_t)(x + w - 1U), y, h);
}

void render_char(uint8_t *fb, uint16_t x, uint16_t y, char c, uint8_t scale)
{
	uint16_t idx;
	uint8_t s = scale ? scale : 1U;

	if ((uint8_t)c < FONT5X5_FIRST || (uint8_t)c > FONT5X5_LAST) {
		c = '?';
	}
	idx = (uint16_t)((uint8_t)c - FONT5X5_FIRST);

	for (uint8_t row = 0; row < FONT5X5_H; row++) {
		uint8_t bits = font5x5[idx * FONT5X5_H + row];

		for (uint8_t col = 0; col < FONT5X5_W; col++) {
			if (bits & (uint8_t)(1U << (4U - col))) {
				for (uint8_t dy = 0; dy < s; dy++) {
					for (uint8_t dx = 0; dx < s; dx++) {
						render_pixel(fb,
							(uint16_t)(x + col * s + dx),
							(uint16_t)(y + row * s + dy),
							true);
					}
				}
			}
		}
	}
}

void render_text(uint8_t *fb, uint16_t x, uint16_t y, const char *s,
		uint8_t scale)
{
	uint8_t s_scale = scale ? scale : 1U;

	while (*s) {
		render_char(fb, x, y, *s, s_scale);
		x = (uint16_t)(x + (FONT5X5_W + 1U) * s_scale);
		s++;
	}
}

uint16_t render_text_width(const char *s, uint8_t scale)
{
	uint8_t s_scale = scale ? scale : 1U;
	size_t len = strlen(s);

	if (len == 0) {
		return 0;
	}
	return (uint16_t)((len * FONT5X5_W + (len - 1U)) * s_scale);
}

uint16_t render_text_right(uint8_t *fb, uint16_t right_x, uint16_t y,
			   const char *s, uint8_t scale)
{
	uint16_t w = render_text_width(s, scale);

	if (w > right_x) {
		render_text(fb, 0, y, s, scale);
		return w;
	}
	render_text(fb, (uint16_t)(right_x - w), y, s, scale);
	return (uint16_t)(right_x - w);
}

void render_text_center(uint8_t *fb, uint16_t x_left, uint16_t x_right,
			uint16_t y, const char *s, uint8_t scale)
{
	uint16_t w = render_text_width(s, scale);
	uint16_t x;

	if (w >= (uint16_t)(x_right - x_left)) {
		render_text(fb, x_left, y, s, scale);
		return;
	}
	x = (uint16_t)(x_left + ((x_right - x_left) - w) / 2U);
	render_text(fb, x, y, s, scale);
}

void render_uint_field(uint8_t *fb, uint16_t x, uint16_t y, uint32_t v,
		       uint8_t width, uint8_t scale)
{
	char buf[16];
	char *p = buf + sizeof(buf);
	uint8_t n = 0;
	uint8_t s_scale = scale ? scale : 1U;

	/* Always render at least one digit. */
	do {
		*--p = (char)('0' + (v % 10U));
		v /= 10U;
		n++;
	} while (v != 0 && n < sizeof(buf));

	while (n < width && n < sizeof(buf)) {
		*--p = ' ';
		n++;
	}
	render_text(fb, x, y, p, s_scale);
}

void render_bar_h(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		  uint8_t pct)
{
	uint16_t inner_w;
	uint16_t inner_h;
	uint16_t fill_w;

	if (w < 2U || h < 2U) {
		return;
	}
	if (pct > 100U) {
		pct = 100U;
	}
	render_rect(fb, x, y, w, h, false);

	inner_w = (uint16_t)(w - 2U);
	inner_h = (uint16_t)(h - 2U);
	fill_w = (uint16_t)((uint32_t)inner_w * pct / 100U);

	for (uint16_t i = 0; i < inner_h; i++) {
		for (uint16_t j = 0; j < fill_w; j++) {
			render_pixel(fb, (uint16_t)(x + 1U + j),
				     (uint16_t)(y + 1U + i), true);
		}
	}
	/* Suppress unused variable warning on inner_h */
	(void)inner_h;
}

void render_sparkline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w,
		      uint16_t h, const uint8_t *samples, uint16_t count,
		      uint8_t max)
{
	uint16_t inner_w;
	uint16_t inner_h;

	if (w < 2U || h < 2U || count == 0U) {
		return;
	}
	render_rect(fb, x, y, w, h, false);

	inner_w = (uint16_t)(w - 2U);
	inner_h = (uint16_t)(h - 2U);

	for (uint16_t i = 0; i < count && i < inner_w; i++) {
		uint8_t v = samples[i];
		uint16_t height_px;

		if (v > max) {
			v = max;
		}
		height_px = (uint16_t)((uint32_t)v * inner_h / max);
		if (height_px == 0U) {
			height_px = 1U;   /* always show at least one pixel */
		}
		for (uint16_t j = 0; j < height_px; j++) {
			render_pixel(fb, (uint16_t)(x + 1U + i),
				     (uint16_t)(y + h - 2U - j), true);
		}
	}
}
