/*
 * (C) Copright 2015 Alan Cox
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This code is extensively based upon level9, the Level 9 interpreter
 * by Glen Summers, David Kinder, Alan Staniforth, Simon Baldwin, Dieter
 * Baron and Andreas Scherrer.
 */


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

/*
 *	Graphics driver for L9X
 *
 *	This will be forked from the main process and own the upper
 *	screen area. Right now it only understands V2 graphics format
 *	and also has no ideas about output, it just generates the four
 *	colour maps.
 *
 *	When we've got the needed bits in place elsewhere we can then
 *	fork this off and send it messages over a pipe to manage the
 *	display. That also means we'll get the fancy behaviour of late
 *	games for free when the graphics ran asynchronously. We will need
 *	slightly more logic because we have to avoid getting further and
 *	further behind, and we also have to handshake with the main game
 *	when switching in and out of graphics mode, probably two pipes then!
 */
#define GFXSTACK_SIZE	64

static char pictures[8192];

static uint8_t *gfxstack_base[GFXSTACK_SIZE];
static uint8_t **gfxstack = gfxstack_base;
static uint16_t gfxscale_base[GFXSTACK_SIZE];	/* Check if byte will do */
static uint16_t *gfxscale = gfxscale_base;

static const uint8_t scalemap[] = {
  0x00, 0x02, 0x04, 0x06, 0x07, 0x09, 0x0c, 0x10
};

static uint16_t scale;
static uint8_t reflect;
static uint8_t option;
static uint8_t ink;
static uint8_t palette[4];
static int16_t draw_x, draw_y, new_x, new_y;

static uint8_t display[128 * 160 / 4];

/* The following routines will probably want to be in asm for any 8bit
   platform to provide sufficient speed */

static void mayplot(uint8_t x, uint8_t y)
{
  uint8_t *byte;
  uint8_t shift = (x & 3) << 1;
  uint8_t b = 3 << shift;
  uint8_t c = ink << shift;
  uint8_t oc = option << shift;

  if (x < 0 || x > 160 || y < 0 || y > 127)
    return;

  x >>= 2;
  byte = display + (y * 160 / 4) + x;
  if ((*byte & b) == oc) {
    *byte &= ~b;
    *byte |= c;
  }
}

static void plot(uint8_t x, uint8_t y, uint8_t c)
{
  uint8_t *byte;
  uint8_t shift = (x & 3) << 1;
  uint8_t b = 3 << shift;
  
  if (x < 0 || x > 160 || y < 0 || y > 127)
    return;

  c <<= shift;;

  x >>= 2;

  byte = display + (y * 160 / 4) + x;
  *byte &= ~b;
  *byte |= c;
}

static uint8_t peek(uint8_t x, uint8_t y)
{
  uint8_t shift = (x & 3) << 1;
  uint8_t *byte = display + (y * 160 / 4) + (x >> 2);
  if (x < 0 || x > 159 || y < 0 || y > 127)
    return 255;
  return (*byte >> shift) & 3;
}

/* Likewise this needs a proper byte filling algorithm with minimal recursion
   or it'll be slow as molasses and eat the stack
   
   Basically you probably want to do
   
   if the byte we are on is entirely the precomputed old colour then set
   it to the precomputed new colour and do a left and right sweep. When doing
   the sweep add any byte above or below that is solid old colour unless you
   saw solid old colour last byte above/below as you sweep.
   
   when you hit bytes left/right fill the end pixels doing a pixel by pixel
   walk.
   
   During the pixel, and byte sweeps when you hit a byte that is not solid old,
   or solid new then add each pixel that doesn't match to the stack
   
*/

static void fill(uint8_t x, uint8_t y, uint8_t c, uint8_t c2)
{
  if (peek(x, y) != c2)
    return;
  plot(x, y, c);
  fill(x+1, y, c, c2);
  fill(x-1, y, c, c2);
  fill(x, y-1, c, c2);
  fill(x, y+1, c, c2);
}

static void line(int16_t x, int16_t y, int16_t x1, int16_t y1)
{
	int8_t stepy = 0;
	int8_t ydir = 1;
	int16_t dx;
	int16_t derr;
	int16_t acc;

	/* Do we need to draw multiple pixels up or across ? */
	if (abs(y1 - y) > abs(x1 - x)) {
		int16_t t = x;
		x = y;
		y = t;
		t = x1;
		x1 = y1;
		y1 = t;
		stepy = 1;
	}
	/* Which way around ? */
	if (x > x1) {
		int16_t t = x1;
		x1 = x;
		x = t;
		t = y1;
		y1 = y;
		y = t;
	}

	/* Work out our step and draw */
	derr = abs(y1 - y);
	dx = x1 - x;
	if (y1 < y) {
		ydir = -1;
	}
	acc = dx >> 1;

	for (; x <= x1; x++) {
		if (stepy)
			mayplot(y, x);	/* Inverted co-ords y is x x is y */
		else
			mayplot(x, y);
		acc -= derr;
		if (acc < 0) {
			acc += dx;
			y += ydir;
		}
	}
}

static void error(const char *p)
{
  fprintf(stderr, "%s\n", p);
  exit(1);
}

static void fill_current(uint8_t i)
{
  uint8_t m = i & 3;
  if (m == 0)
    m = ink;
  fill(draw_x >> 6 , 127 - (draw_y >> 7), m, option & 3);
}

static void draw_line(void)
{
  line(draw_x >> 6, 127 - (draw_y >> 7), new_x >> 6, 127 - (new_y >> 7));
}

static uint8_t *gfind(uint16_t code)
{	
  uint8_t *p = pictures;
  uint8_t h = code >> 4;
  uint8_t l = (code & 0x0F) << 4;
  
  while(1) {
    /* Top 8bits of code */
    uint8_t c = *p++;
    /* Next 4 bits of code, then 4 high bits of length */
    uint8_t cl = *p++;
    /* Check for end mark */
    if (c & 0x80)
      return NULL;
    if (c == h) {
      if ((cl & 0xF0) == l)
        return p + 1;
    }
    /* Move on to next record */
    p  += ((cl & 0x0F) << 8) | *p - 2;
  }
}
    
static void gfexecute(uint8_t * pc)
{
  static int16_t x, y;
  
  if (pc == NULL)
    return;

  while (1) {
    uint8_t opcode = *pc++;
    uint8_t draw = 0;

    switch (opcode >> 6) {
    case 0:
      draw = 1;
    case 1:
      /* 0 - draw 1 - move */
      x = (opcode & 0x18) >> 3;
      if (opcode & 0x20)
        x = (x | 0xFC) - 0x100;
      y = (opcode & 0x03) << 2;
      if (opcode & 0x04)
        y = (y | 0xF0) - 0x100;
      if (reflect & 2)
        x = -x;
      if (reflect & 1)
        y = -y;
      new_x = draw_x + ((x * scale) & ~7);
      new_y = draw_y + ((y * scale) & ~7);
      if (draw)
        draw_line();
      draw_x = new_x;
      draw_y = new_y;
      break;
    case 2:
      *gfxstack++ = pc;
      *gfxscale++ = scale;
      pc = gfind(opcode & 0x3F);
      break;
    case 3:
      switch ((opcode >> 3) & 7) {
      case 0:
	draw = 1;
      case 1:
      {
        uint16_t coord = ((uint16_t)opcode << 8) | *pc++;
        x = (coord & 0x3E0) >> 5;
        if (coord & 0x400)
          x = (x | 0xE0) - 0x100;
        y = (coord & 0x0F) << 2;
        if (coord & 0x10)
          y = (y | 0xC0) - 0x100;
        if (reflect & 2)
          x = -x;
        if (reflect & 1)
          y = -y;
        new_x = draw_x + ((x * scale) & ~7);
        new_y = draw_y + ((y * scale) & ~7);
        if (draw)
          draw_line();
        draw_x = new_x;
        draw_y = new_y;
      }
      break;
      case 2:
	ink = opcode & 3;
	break;
      case 3:
	opcode &= 7;
	if (!opcode) {
	  scale = 0x80;
	  /* Early games only */
	  gfxscale = gfxscale_base;
        } else {
          uint16_t ns = (scale * scalemap[opcode]) >> 3;
          if (ns > 0xff) {
            printf("SCALE OVERFLOW\n");
            ns = 0xff;
          }
          scale = ns;
        }
        break;
      case 4:
	fill_current(opcode & 7);
	break;
      case 5:
        *gfxstack++ = pc + 1;
        *gfxscale++ = scale;
        pc = gfind(((((uint16_t)opcode) & 7) << 8) | *pc);
	break;
      case 6:
        if (opcode & 4) {
          opcode &= 3;
          opcode ^= reflect;
        }
        reflect = opcode;
        break;
      case 7:
	switch (opcode & 7) {
	case 1:
	  opcode = *pc++;
	  palette[(opcode >> 3) & 3] = opcode & 7;
	  break;
	case 3:
	  draw_x = 0x40 * *pc++;
	  draw_y = 0x40 * *pc++;
	  break;
	case 4:
	  option = *pc ? ((*pc & 3) | 0x80) : 0;
	  pc++;
	  break;
	case 7:
	  if (gfxstack == gfxstack_base)
	    return;
	  pc = *--gfxstack;
          /* Fall through */
	case 5:
	  if (gfxscale != gfxscale_base)
	    scale = *--gfxscale;
          break;
	default:
	  error("ILGFX");
	}
      }
    }
  }
}

static void draw_picture(uint16_t pic)
{
  ink = 3;
  option = 0;
  reflect = 0;
  draw_x = 0x1400;
  draw_y = 0x1400;
  scale = 0x80;
  gfxstack = gfxstack_base;
  gfxscale = gfxscale_base;
  gfexecute(gfind(0));
  gfexecute(gfind(pic));
}

static const char *palmap[8] = {
  "0 0 0",
  "255 0 0",
  "0 255 0",	/* Some machines have a rather saner green */
  "255 255 0",
  "0 0 255",
  "255 0 255",	/* Magenta or brown, brown is better ! */
  "0 255 255",
  "255 255 255"
};

static void printrgb(FILE *o, uint8_t c)
{
  fprintf(o, "%s ", palmap[palette[c]]);
}

static void write_ppm(FILE *o)
{
	uint8_t c;
	uint8_t x, y;

	fprintf(o, "P3\n160 128 3\n");

	for (y = 0; y < 128; y++) {
		for (x = 0; x < 160; x++) {
		  c = peek(x, y);
		  printrgb(o, c);
		}
		fprintf(o, "\n");
	}
}

int main(int argc, char *argv[])
{
  int fd;
  FILE *o;

  if (argc != 3) {
    fprintf(stderr, "g9x: picturefile number\n");
    exit(1);
  }
  fd = open(argv[1], O_RDONLY);
  if (fd == -1) {
    perror(argv[1]);
    exit(1);
  }
  if (read(fd, pictures, sizeof(pictures)) < 1024) {
    fprintf(stderr, "Invalid picture data.\n");
    exit(1);
  }
  close(fd);
  draw_picture(atoi(argv[2]));

  o = fopen("out.ppm", "w");
  if (o == NULL) {
    perror("out.ppm");
    exit(1);
  }
  write_ppm(o);
  fclose(o);
  return 0;
}

  