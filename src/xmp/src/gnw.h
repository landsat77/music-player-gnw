/*
 * Game & Watch changes of libxmp-lite (LIBXMP_GNW): the samples of the
 * modules too big for the RAM of the console live in its external flash.
 *
 * The player (src/app/mp_player.c) loads such a module twice:
 *  1. with XMP_SMPCTL_SKIP: no sample data (compressed IT samples are not
 *     decompressed either), the sizes give the room to reserve;
 *  2. with libxmp_gnw_flash set: WRITE streams each sample to the flash,
 *     converted on the way like libxmp_load_sample() converts it in RAM;
 *     READ (the samples are already there) only points each sample at its
 *     place in the flash. Both walk the samples in the same order, so the
 *     offsets are the same.
 * A sample takes libxmp_gnw_slot_size() bytes: its 4 guard bytes, its
 * data, its extra frames (interpolation), padding to 4 bytes.
 */
#ifndef LIBXMP_GNW_H
#define LIBXMP_GNW_H

/* libxmp_load_sample(): `buffer` (NOLOAD) is the data of a block the
 * caller allocated (bytelen + extralen + 4 bytes, buffer = block + 4):
 * the sample takes it over, no copy. Mono samples only. */
#define SAMPLE_FLAG_GNW_ADOPT	0x10000
/* libxmp_load_sample(), flash WRITE: the sample is IT-compressed at the
 * file position (`buffer`: the loader's temporary buffer) */
#define SAMPLE_FLAG_GNW_ITCOMP	0x20000
#define SAMPLE_FLAG_GNW_IT215	0x40000
#define LIBXMP_GNW_IT_TMPLEN	65536

enum {
	LIBXMP_GNW_WRITE = 1,
	LIBXMP_GNW_READ = 2
};

struct libxmp_gnw_flash {
	int mode;			/* LIBXMP_GNW_WRITE / READ */
	const unsigned char *base;	/* where the blob is (mapped flash) */
	unsigned int offset;		/* next sample slot */
	unsigned int total;		/* size of the blob */
	/* WRITE: appends to the blob, 0 on error */
	int (*append)(void *opaque, const void *buf, unsigned int len);
	void *opaque;
	int error;
};

/* set by the player while it loads a big module (NULL: samples in RAM) */
extern struct libxmp_gnw_flash *libxmp_gnw_flash;

/* the blob of the module playing (libxmp_free_sample() does not free its
 * samples, the "invert loop" effect does not write them) */
extern const unsigned char *libxmp_gnw_flash_lo, *libxmp_gnw_flash_hi;
#define libxmp_gnw_flash_owns(p) \
	((const unsigned char *)(p) >= libxmp_gnw_flash_lo && \
	 (const unsigned char *)(p) < libxmp_gnw_flash_hi)

/* bytes a sample of `len` frames with these XMP_SAMPLE_* flags takes in
 * the blob (0: not loaded) */
unsigned int libxmp_gnw_slot_size(int len, int flg);

#endif
