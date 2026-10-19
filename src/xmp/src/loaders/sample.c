/* Extended Module Player
 * Copyright (C) 1996-2024 Claudio Matsuoka and Hipolito Carraro Jr
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "../common.h"
#include "loader.h"
#if defined(LIBXMP_GNW) && !defined(LIBXMP_CORE_DISABLE_IT)
#include "it.h"
#endif

#ifndef LIBXMP_CORE_PLAYER
/*
 * From the Audio File Formats (version 2.5)
 * Submitted-by: Guido van Rossum <guido@cwi.nl>
 * Last-modified: 27-Aug-1992
 *
 * The Acorn Archimedes uses a variation on U-LAW with the bit order
 * reversed and the sign bit in bit 0.  Being a 'minority' architecture,
 * Arc owners are quite adept at converting sound/image formats from
 * other machines, and it is unlikely that you'll ever encounter sound in
 * one of the Arc's own formats (there are several).
 */
static const int8 vdic_table[128] = {
	/*   0 */	  0,   0,   0,   0,   0,   0,   0,   0,
	/*   8 */	  0,   0,   0,   0,   0,   0,   0,   0,
	/*  16 */	  0,   0,   0,   0,   0,   0,   0,   0,
	/*  24 */	  1,   1,   1,   1,   1,   1,   1,   1,
	/*  32 */	  1,   1,   1,   1,   2,   2,   2,   2,
	/*  40 */	  2,   2,   2,   2,   3,   3,   3,   3,
	/*  48 */	  3,   3,   4,   4,   4,   4,   5,   5,
	/*  56 */	  5,   5,   6,   6,   6,   6,   7,   7,
	/*  64 */	  7,   8,   8,   9,   9,  10,  10,  11,
	/*  72 */	 11,  12,  12,  13,  13,  14,  14,  15,
	/*  80 */	 15,  16,  17,  18,  19,  20,  21,  22,
	/*  88 */	 23,  24,  25,  26,  27,  28,  29,  30,
	/*  96 */	 31,  33,  34,  36,  38,  40,  42,  44,
	/* 104 */	 46,  48,  50,  52,  54,  56,  58,  60,
	/* 112 */	 62,  65,  68,  72,  77,  80,  84,  91,
	/* 120 */	 95,  98, 103, 109, 114, 120, 126, 127
};

/* Convert 7 bit samples to 8 bit */
static void convert_7bit_to_8bit(uint8 *p, int l)
{
	for (; l--; p++) {
		*p <<= 1;
	}
}

/* Convert Archimedes VIDC samples to linear */
static void convert_vidc_to_linear(uint8 *p, int l)
{
	int i;
	int8 amp;
	uint8 x;

	for (i = 0; i < l; i++) {
		x = p[i];
		amp = vdic_table[x >> 1];
		p[i] = (uint8)((x & 0x01) ? -amp : amp);
	}
}

static void adpcm4_decoder(uint8 *inp, uint8 *outp, char *tab, int len)
{
	char delta = 0;
	uint8 b0, b1;
	int i;

	len = (len + 1) / 2;

	for (i = 0; i < len; i++) {
		b0 = *inp;
		b1 = *inp++ >> 4;
		delta += tab[b0 & 0x0f];
		*outp++ = delta;
		delta += tab[b1 & 0x0f];
		*outp++ = delta;
	}
}
#endif

/* Convert differential to absolute sample data */
static void convert_delta(uint8 *p, int frames, int is_16bit, int channels)
{
	uint16 *w = (uint16 *)p;
	uint16 absval;
	int chn, i;

	if (is_16bit) {
		for (chn = 0; chn < channels; chn++) {
			absval = 0;
			for (i = 0; i < frames; i++) {
				absval = *w + absval;
				*w++ = absval;
			}
		}
	} else {
		for (chn = 0; chn < channels; chn++) {
			absval = 0;
			for (i = 0; i < frames; i++) {
				absval = *p + absval;
				*p++ = (uint8) absval;
			}
		}
	}
}

/* Convert signed to unsigned sample data */
static void convert_signal(uint8 *p, int l, int r)
{
	uint16 *w = (uint16 *)p;

	if (r) {
		for (; l--; w++)
			*w += 0x8000;
	} else {
		for (; l--; p++)
			*p += (unsigned char)0x80;
	}
}

/* Convert little-endian 16 bit samples to big-endian */
static void convert_endian(uint8 *p, int l)
{
	uint8 b;
	int i;

	for (i = 0; i < l; i++) {
		b = p[0];
		p[0] = p[1];
		p[1] = b;
		p += 2;
	}
}

/* Convert non-interleaved stereo to interleaved stereo.
 * Due to tracker quirks this should be done after delta decoding, etc. */
static void convert_stereo_interleaved(void * LIBXMP_RESTRICT _out,
 const void *in, int frames, int is_16bit)
{
	int i;

	if (is_16bit) {
		const int16 *in_l = (const int16 *)in;
		const int16 *in_r = in_l + frames;
		int16 *out = (int16 *)_out;

		for (i = 0; i < frames; i++) {
			*(out++) = *(in_l++);
			*(out++) = *(in_r++);
		}
	} else {
		const uint8 *in_l = (const uint8 *)in;
		const uint8 *in_r = in_l + frames;
		uint8 *out = (uint8 *)_out;

		for (i = 0; i < frames; i++) {
			*(out++) = *(in_l++);
			*(out++) = *(in_r++);
		}
	}
}


#ifdef LIBXMP_GNW
/* ------------------------------------------------------------------ */
/* Game & Watch: samples in the external flash (gnw.h)                 */
/* ------------------------------------------------------------------ */
struct libxmp_gnw_flash *libxmp_gnw_flash;
const unsigned char *libxmp_gnw_flash_lo, *libxmp_gnw_flash_hi;

unsigned int libxmp_gnw_slot_size(int len, int flg)
{
	unsigned int bytelen, extralen;

	if (len <= 0 || len > MAX_SAMPLE_SIZE)
		return 0;
	bytelen = len;
	extralen = 4;
	if (flg & XMP_SAMPLE_16BIT) {
		bytelen *= 2;
		extralen *= 2;
	}
	if (flg & XMP_SAMPLE_STEREO) {
		bytelen *= 2;
		extralen *= 2;
	}
	return (4 + bytelen + extralen + 3) & ~3u;
}

static int gnw_append(const void *buf, unsigned int len)
{
	struct libxmp_gnw_flash *g = libxmp_gnw_flash;

	if (g->error)
		return -1;
	if (len && !g->append(g->opaque, buf, len)) {
		g->error = 1;
		return -1;
	}
	return 0;
}

static int gnw_zeros(unsigned int len)
{
	static const uint8 zero[64];

	while (len) {
		unsigned int n = len > sizeof(zero) ? sizeof(zero) : len;
		if (gnw_append(zero, n) < 0)
			return -1;
		len -= n;
	}
	return 0;
}

/* the place of the next sample in the blob (data pointer: after its 4
 * guard bytes) */
static unsigned char *gnw_place(unsigned int slot)
{
	struct libxmp_gnw_flash *g = libxmp_gnw_flash;
	unsigned char *p;

	if (g->offset + slot > g->total) {
		g->error = 1;
		return NULL;
	}
	p = (unsigned char *)g->base + g->offset + 4;
	g->offset += slot;
	return p;
}

/* The conversions of libxmp_load_sample() (endianness, delta, unsigned),
 * one chunk at a time: the delta accumulator goes on from chunk to chunk
 * and starts again for each channel (convert_delta()). */
struct gnw_conv {
	int is16, swap, delta, uns;	/* delta: 1 = DIFF, 2 = 8BDIFF */
	unsigned int count, reset, period;
	uint16 acc;
};

static void gnw_conv_init(struct gnw_conv *c, int flags, struct xmp_sample *xxs)
{
	memset(c, 0, sizeof(*c));
	c->is16 = !!(xxs->flg & XMP_SAMPLE_16BIT);
#ifdef WORDS_BIGENDIAN
	c->swap = c->is16 && (~flags & SAMPLE_FLAG_BIGEND);
#else
	c->swap = c->is16 && (flags & SAMPLE_FLAG_BIGEND);
#endif
	if (flags & SAMPLE_FLAG_DIFF) {
		c->delta = 1;
		c->period = xxs->len;
	} else if (flags & SAMPLE_FLAG_8BDIFF) {
		c->delta = 2;
		c->period = c->is16 ? xxs->len * 2 : xxs->len;
	}
	c->reset = 0;
	c->uns = !!(flags & SAMPLE_FLAG_UNS);
}

static void gnw_convert(struct gnw_conv *c, uint8 *p, unsigned int n)
{
	unsigned int i;

	if (c->swap) {
		for (i = 0; i + 1 < n; i += 2) {
			uint8 b = p[i];
			p[i] = p[i + 1];
			p[i + 1] = b;
		}
	}
	if (c->delta == 1 && c->is16) {
		for (i = 0; i + 1 < n; i += 2) {
			uint16 w = (uint16)(p[i] | (p[i + 1] << 8));
			if (c->count == c->reset) {
				c->acc = 0;
				c->reset += c->period;
			}
			c->acc = (uint16)(w + c->acc);
			p[i] = (uint8)c->acc;
			p[i + 1] = (uint8)(c->acc >> 8);
			c->count++;
		}
	} else if (c->delta) {
		for (i = 0; i < n; i++) {
			if (c->count == c->reset) {
				c->acc = 0;
				c->reset += c->period;
			}
			c->acc = (uint8)(p[i] + c->acc);
			p[i] = (uint8)c->acc;
			c->count++;
		}
	}
	if (c->uns) {
		if (c->is16) {
			for (i = 0; i + 1 < n; i += 2)
				p[i + 1] ^= 0x80;	/* += 0x8000 */
		} else {
			for (i = 0; i < n; i++)
				p[i] += 0x80;
		}
	}
}

/* writes one sample: guard bytes, the data fed in chunks, extra frames */
struct gnw_out {
	struct gnw_conv conv;
	unsigned int framelen, bytelen, extralen, slot, written;
	uint8 last[4];
	int head_done;
};

static int gnw_out_feed(struct gnw_out *o, uint8 *p, unsigned int n)
{
	unsigned int i;

	if (n > o->bytelen - o->written)
		n = o->bytelen - o->written;
	if (n == 0)
		return 0;
	gnw_convert(&o->conv, p, n);
	if (!o->head_done) {
		/* data[-1..-4] = data[framelen - 1 .. framelen - 4] */
		uint8 head[8];
		int k;
		for (i = 0; i < o->framelen; i++)
			head[4 + i] = p[i];	/* n >= framelen */
		for (k = -1; k >= -4; k--)
			head[4 + k] = head[4 + o->framelen + k];
		if (gnw_append(head, 4) < 0)
			return -1;
		o->head_done = 1;
	}
	if (gnw_append(p, n) < 0)
		return -1;
	/* the last frame (framelen <= 4, chunks are whole frames) */
	if (n >= o->framelen)
		memcpy(o->last, p + n - o->framelen, o->framelen);
	o->written += n;
	return 0;
}

static int gnw_out_end(struct gnw_out *o)
{
	uint8 tail[16];
	unsigned int i;

	/* data[bytelen + i] = data[bytelen - framelen + i] */
	for (i = 0; i < o->extralen; i++)
		tail[i] = i < o->framelen ? o->last[i] : tail[i - o->framelen];
	if (gnw_append(tail, o->extralen) < 0)
		return -1;
	return gnw_zeros(o->slot - (4 + o->bytelen + o->extralen));
}

#define GNW_CHUNK 4096

static int gnw_flash_sample(struct module_data *m, HIO_HANDLE *f, int flags,
			    struct xmp_sample *xxs, const void *buffer,
			    int bytelen, int framelen, int extralen, int channels)
{
	struct libxmp_gnw_flash *g = libxmp_gnw_flash;
	unsigned int slot = (4 + bytelen + extralen + 3) & ~3u;
	struct gnw_out o;
	uint8 *chunk;
	int ret = 0;

	if (flags & SAMPLE_FLAG_FULLREP) {
		if (xxs->lps == 0 && xxs->len > xxs->lpe)
			xxs->flg |= XMP_SAMPLE_LOOP_FULL;
	}

	if (g->mode == LIBXMP_GNW_READ) {
		/* already there: only skip the data in the file */
		if ((~flags & SAMPLE_FLAG_NOLOAD) && (~flags & SAMPLE_FLAG_GNW_ITCOMP))
			hio_seek(f, bytelen, SEEK_CUR);
		xxs->data = gnw_place(slot);
		return xxs->data ? 0 : -1;
	}

	/* Non-interleaved stereo: converted in RAM like the stock code,
	 * then copied (rare: IT / some XM stereo samples) */
	if (channels == 2 && (~flags & SAMPLE_FLAG_INTERLEAVED)) {
		libxmp_gnw_flash = NULL;
		ret = libxmp_load_sample(m, f, flags & ~SAMPLE_FLAG_FULLREP, xxs, buffer);
		libxmp_gnw_flash = g;
		if (ret < 0 || xxs->data == NULL)
			return ret;
		if (gnw_append(xxs->data - 4, 4 + bytelen + extralen) < 0 ||
		    gnw_zeros(slot - (4 + bytelen + extralen)) < 0) {
			libxmp_free_sample(xxs);
			return -1;
		}
		free(xxs->data - 4);
		xxs->data = gnw_place(slot);
		return xxs->data ? 0 : -1;
	}

	memset(&o, 0, sizeof(o));
	gnw_conv_init(&o.conv, flags, xxs);
	o.framelen = framelen;
	o.bytelen = bytelen;
	o.extralen = extralen;
	o.slot = slot;

	if (flags & SAMPLE_FLAG_GNW_ITCOMP) {
		/* compressed IT sample (mono): one block at a time */
		int is16 = !!(xxs->flg & XMP_SAMPLE_16BIT);
		int it215 = !!(flags & SAMPLE_FLAG_GNW_IT215);
		int left = xxs->len;
		uint8 *tmp = (uint8 *)buffer;	/* TEMP_BUFFER_LEN bytes */
		chunk = (uint8 *) malloc(0x8000);
		if (chunk == NULL)
			return -1;
		while (left > 0 && ret == 0) {
			int n = left > (is16 ? 0x4000 : 0x8000) ? (is16 ? 0x4000 : 0x8000) : left;
			memset(chunk, 0, 0x8000);
			if (is16)
				ret = itsex_decompress16(f, (int16 *)chunk, n, tmp, LIBXMP_GNW_IT_TMPLEN, it215);
			else
				ret = itsex_decompress8(f, chunk, n, tmp, LIBXMP_GNW_IT_TMPLEN, it215);
			if (ret == 0)
				ret = gnw_out_feed(&o, chunk, is16 ? n * 2 : n);
			left -= n;
		}
	} else {
		chunk = (uint8 *) malloc(GNW_CHUNK);
		if (chunk == NULL)
			return -1;
		while (o.written < o.bytelen && ret == 0) {
			unsigned int n = o.bytelen - o.written;
			if (n > GNW_CHUNK)
				n = GNW_CHUNK;
			if (flags & SAMPLE_FLAG_NOLOAD) {
				memcpy(chunk, (const uint8 *)buffer + o.written, n);
			} else {
				int x = hio_read(chunk, 1, n, f);
				if (x < 0)
					x = 0;
				if ((unsigned int)x < n)	/* short read: zeros */
					memset(chunk + x, 0, n - x);
			}
			ret = gnw_out_feed(&o, chunk, n);
		}
	}
	free(chunk);
	if (ret == 0 && o.written < o.bytelen) {
		/* a broken compressed stream: zeros */
		uint8 z[256];
		memset(z, 0, sizeof(z));
		while (ret == 0 && o.written < o.bytelen) {
			unsigned int n = o.bytelen - o.written;
			ret = gnw_out_feed(&o, z, n > sizeof(z) ? sizeof(z) : n);
		}
	}
	if (ret == 0)
		ret = gnw_out_end(&o);
	if (ret < 0)
		return -1;
	xxs->data = gnw_place(slot);
	return xxs->data ? 0 : -1;
}
#endif

int libxmp_load_sample(struct module_data *m, HIO_HANDLE *f, int flags, struct xmp_sample *xxs, const void *buffer)
{
	unsigned char *tmp = NULL;
	unsigned char *dest;
	int channels = 1;
	int framelen;
	int bytelen, extralen, i;

#ifndef LIBXMP_CORE_PLAYER
	/* Adlib FM patches */
	if (flags & SAMPLE_FLAG_ADLIB) {
		return 0;
	}
#endif

	/* Empty or invalid samples
	 */
	if (xxs->len <= 0) {
		return 0;
	}

	/* Skip sample loading
	 * FIXME: fails for ADPCM samples
	 *
	 * + Sanity check: skip huge samples (likely corrupt module)
	 */
	if (xxs->len > MAX_SAMPLE_SIZE || (m && m->smpctl & XMP_SMPCTL_SKIP)) {
		if (~flags & SAMPLE_FLAG_NOLOAD) {
			/* coverity[check_return] */
			hio_seek(f, xxs->len, SEEK_CUR);
		}
		return 0;
	}

	/* Patches with samples
	 * Allocate extra sample for interpolation.
	 */
	bytelen = xxs->len;
	framelen = 1;
	extralen = 4;

	if (xxs->flg & XMP_SAMPLE_16BIT) {
		bytelen *= 2;
		extralen *= 2;
		framelen *= 2;
	}
	if (xxs->flg & XMP_SAMPLE_STEREO) {
		bytelen *= 2;
		extralen *= 2;
		framelen *= 2;
		channels = 2;
	}

	/* If this sample starts at or after EOF, skip it entirely.
	 */
#ifdef LIBXMP_GNW
	/* compressed IT samples: the caller checked the file size */
	if ((~flags & SAMPLE_FLAG_NOLOAD) && (~flags & SAMPLE_FLAG_GNW_ITCOMP)) {
#else
	if (~flags & SAMPLE_FLAG_NOLOAD) {
#endif
		long file_pos, file_len;
		long remaining = 0;
		long over = 0;
		if (!f) {
			return 0;
		}
		file_pos = hio_tell(f);
		file_len = hio_size(f);
		if (file_pos >= file_len) {
			D_(D_WARN "ignoring sample at EOF");
			return 0;
		}
		/* If this sample goes past EOF, truncate it. */
		remaining = file_len - file_pos;
#ifndef LIBXMP_CORE_PLAYER
		if (flags & SAMPLE_FLAG_ADPCM) {
			long bound = 16 + ((bytelen + 1) >> 1);
			if (remaining < 16) {
				D_(D_WARN "ignoring truncated ADPCM sample");
				return 0;
			}
			if (bound > remaining) {
				over = bound - remaining;
				bytelen = (remaining - 16) << 1;
			}
		} else
#endif
		if (bytelen > remaining) {
			over = bytelen - remaining;
			bytelen = remaining;
		}

		if (over) {
			D_(D_WARN "sample would extend %ld bytes past EOF; truncating to %ld",
				over, remaining);

			/* Trim extra bytes non-aligned to sample frame. */
			bytelen -= bytelen & (framelen - 1);

			xxs->len = bytelen;
			if (xxs->flg & XMP_SAMPLE_16BIT)
				xxs->len >>= 1;
			if (xxs->flg & XMP_SAMPLE_STEREO)
				xxs->len >>= 1;
		}
	}

	/* Loop parameters sanity check
	 */
	if (xxs->lps < 0) {
		xxs->lps = 0;
	}
	if (xxs->lpe > xxs->len) {
		xxs->lpe = xxs->len;
	}
	if (xxs->lps >= xxs->len || xxs->lps >= xxs->lpe) {
		xxs->lps = xxs->lpe = 0;
		xxs->flg &= ~(XMP_SAMPLE_LOOP | XMP_SAMPLE_LOOP_BIDIR);
	}

	/* Disable bidirectional loop flag if sample is not looped
	 */
	if (xxs->flg & XMP_SAMPLE_LOOP_BIDIR) {
		if (~xxs->flg & XMP_SAMPLE_LOOP)
			xxs->flg &= ~XMP_SAMPLE_LOOP_BIDIR;
	}
	if (xxs->flg & XMP_SAMPLE_SLOOP_BIDIR) {
		if (~xxs->flg & XMP_SAMPLE_SLOOP)
			xxs->flg &= ~XMP_SAMPLE_SLOOP_BIDIR;
	}

#ifdef LIBXMP_GNW
	/* the samples of a big module: in the flash */
	if (libxmp_gnw_flash) {
		return gnw_flash_sample(m, f, flags, xxs, buffer,
					bytelen, framelen, extralen, channels);
	}

	if (flags & SAMPLE_FLAG_GNW_ADOPT) {
		/* the caller's block: buffer = block + 4 (mono only) */
		xxs->data = (unsigned char *)buffer - 4;
	} else
#endif
	/* add guard bytes before the buffer for higher order interpolation */
	xxs->data = (unsigned char *) malloc(bytelen + extralen + 4);
	if (xxs->data == NULL) {
		goto err;
	}

	*(uint32 *)xxs->data = 0;
	xxs->data += 4;
	dest = xxs->data;

	/* If this is a non-interleaved stereo sample, most conversions need
	 * to occur in an intermediate buffer prior to interleaving. Most
	 * formats supporting stereo samples use non-interleaved stereo.
	 */
	if ((xxs->flg & XMP_SAMPLE_STEREO) && (~flags & SAMPLE_FLAG_INTERLEAVED)) {
		tmp = (unsigned char *) malloc(bytelen);
		if (!tmp)
			goto err2;

		dest = tmp;
	}

	if (flags & SAMPLE_FLAG_NOLOAD) {
#ifdef LIBXMP_GNW
		if (dest != buffer)	/* adopted: already in place */
#endif
		memcpy(dest, buffer, bytelen);
	} else
#ifndef LIBXMP_CORE_PLAYER
	if (flags & SAMPLE_FLAG_ADPCM) {
		int x2 = (bytelen + 1) >> 1;
		char table[16];

		if (hio_read(table, 1, 16, f) != 16) {
			goto err2;
		}
		if (hio_read(dest + x2, 1, x2, f) != x2) {
			goto err2;
		}
		adpcm4_decoder((uint8 *)dest + x2,
			       (uint8 *)dest, table, bytelen);
	} else
#endif
	{
		int x = hio_read(dest, 1, bytelen, f);
		if (x != bytelen) {
			D_(D_WARN "short read (%d) in sample load", x - bytelen);
			memset(dest + x, 0, bytelen - x);
		}
	}

#ifndef LIBXMP_CORE_PLAYER
	if (flags & SAMPLE_FLAG_7BIT) {
		convert_7bit_to_8bit(dest, xxs->len * channels);
	}
#endif

	/* Fix endianism if needed */
	if (xxs->flg & XMP_SAMPLE_16BIT) {
#ifdef WORDS_BIGENDIAN
		if (~flags & SAMPLE_FLAG_BIGEND)
			convert_endian(dest, xxs->len * channels);
#else
		if (flags & SAMPLE_FLAG_BIGEND)
			convert_endian(dest, xxs->len * channels);
#endif
	}

	/* Convert delta samples */
	if (flags & SAMPLE_FLAG_DIFF) {
		convert_delta(dest, xxs->len, xxs->flg & XMP_SAMPLE_16BIT, channels);
	} else if (flags & SAMPLE_FLAG_8BDIFF) {
		int len = xxs->len;
		if (xxs->flg & XMP_SAMPLE_16BIT) {
			len *= 2;
		}
		convert_delta(dest, len, 0, channels);
	}

	/* Convert samples to signed */
	if (flags & SAMPLE_FLAG_UNS) {
		convert_signal(dest, xxs->len * channels,
				xxs->flg & XMP_SAMPLE_16BIT);
	}

#ifndef LIBXMP_CORE_PLAYER
	if (flags & SAMPLE_FLAG_VIDC) {
		convert_vidc_to_linear(dest, xxs->len * channels);
	}
#endif

	/* Done converting individual samples; convert to interleaved. */
	if ((xxs->flg & XMP_SAMPLE_STEREO) && (~flags & SAMPLE_FLAG_INTERLEAVED)) {
		convert_stereo_interleaved(xxs->data, dest, xxs->len,
					   xxs->flg & XMP_SAMPLE_16BIT);
	}

	/* Check for full loop samples */
	if (flags & SAMPLE_FLAG_FULLREP) {
	    if (xxs->lps == 0 && xxs->len > xxs->lpe)
		xxs->flg |= XMP_SAMPLE_LOOP_FULL;
	}

	/* Add extra samples at end */
	for (i = 0; i < extralen; i++) {
		xxs->data[bytelen + i] = xxs->data[bytelen - framelen + i];
	}

	/* Add extra samples at start */
	for (i = -1; i >= -4; i--) {
		xxs->data[i] = xxs->data[framelen + i];
	}

	free(tmp);
	return 0;

    err2:
	libxmp_free_sample(xxs);
	free(tmp);
    err:
	return -1;
}

void libxmp_free_sample(struct xmp_sample *s)
{
#ifdef LIBXMP_GNW
	if (s->data && libxmp_gnw_flash_owns(s->data)) {
		s->data = NULL;	/* in the flash */
		return;
	}
#endif
	if (s->data) {
		free(s->data - 4);
		s->data = NULL;		/* prevent double free in PCM load error */
	}
}
