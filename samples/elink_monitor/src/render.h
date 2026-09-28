/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ELINK_MONITOR_RENDER_H
#define ELINK_MONITOR_RENDER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Renderer for the Pervasive E2271CS091 frame memory as set up by the elink
 * sample: 5808 bytes, indexed as [line * 22 + px/8], bit (0x80 >> (px%8)) is
 * pixel px of line. Here we call those "x" (px in the data direction) and "y"
 * (line in the scan direction), so the canvas is x in [0..171], y in [0..263]
 * -- the panel's frame memory is 176 px wide and 264 lines tall, with both
 * sides reading like a small 2.7" portrait bitmap before the COG transposes it
 * onto the 264x176 glass.
 */

/* Whole-canvas helpers */
void render_clear(uint8_t *fb);
void render_fill(uint8_t *fb);

/* Primitives. All coordinates are pixel positions on the canvas; primitives
 * silently clip on the edges so callers don't need to bound-check. */
void render_pixel(uint8_t *fb, uint16_t x, uint16_t y, bool on);
void render_hline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w);
void render_vline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t h);
void render_rect(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		 bool filled);

/* 5x5 text, scale 1..3 (each font pixel becomes an SxS block). */
void render_char(uint8_t *fb, uint16_t x, uint16_t y, char c, uint8_t scale);
void render_text(uint8_t *fb, uint16_t x, uint16_t y, const char *s,
		 uint8_t scale);

/* Right-aligned text, returns the x position used. */
uint16_t render_text_right(uint8_t *fb, uint16_t right_x, uint16_t y,
			   const char *s, uint8_t scale);

/* Centre-aligned text inside [x_left..x_right]. */
void render_text_center(uint8_t *fb, uint16_t x_left, uint16_t x_right,
			uint16_t y, const char *s, uint8_t scale);

/* Render a decimal uint at scale s, with width = total chars including
 * leading spaces. Right-justified in the field. */
void render_uint_field(uint8_t *fb, uint16_t x, uint16_t y, uint32_t v,
		       uint8_t width, uint8_t scale);

/* Width of a text string at the given scale, in pixels. */
uint16_t render_text_width(const char *s, uint8_t scale);

/* Horizontal bar of width w, height h, filled to pct percent of its inner
 * area (border + interior filled to inner_w * pct / 100). */
void render_bar_h(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
		  uint8_t pct);

/* Sparkline: plot a circular buffer of `count` uint8_t samples (range 0..max)
 * into a box of width w, height h, oldest sample on the left. */
void render_sparkline(uint8_t *fb, uint16_t x, uint16_t y, uint16_t w,
		      uint16_t h, const uint8_t *samples, uint16_t count,
		      uint8_t max);

#endif /* ELINK_MONITOR_RENDER_H */
