#ifndef GME_TYPES_H
#define GME_TYPES_H

/*
 * This is a default gme_types.h for use when *not* using
 * CMake.  If CMake is in use gme_types.h.in will be
 * processed instead.
 */
/* Game & Watch: GYM, HES (PC Engine), KSS (MSX) and SAP (Atari) are left
 * out (their sources too), to keep the code small */
#define USE_GME_AY
#define USE_GME_GBS
#define USE_GME_NSF
#define USE_GME_NSFE
#define USE_GME_SPC
/* VGM and VGZ are a package deal */
#define USE_GME_VGM

#endif /* GME_TYPES_H */
