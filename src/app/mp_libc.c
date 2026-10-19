/*
 * The few C library functions the decoders use that the firmware ABI does
 * not export (console only).
 */
#include <stddef.h>
#include <string.h>

#include "mp_platform.h"
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

#include "rg_storage.h"

/* natural logarithm (libxmp: periods <-> notes): x = m * 2^e, m in
 * [sqrt(1/2), sqrt(2)), log(m) = 2 atanh((m - 1) / (m + 1)) */
double log(double x)
{
    if (!(x > 0))
        return x == 0 ? -1.0 / 0.0 : 0.0 / 0.0;
    union { double d; uint64_t u; } v = { x };
    int e = (int)((v.u >> 52) & 0x7ff);
    if (e == 0) { /* subnormal */
        v.d *= 4503599627370496.0; /* 2^52 */
        e = (int)((v.u >> 52) & 0x7ff) - 52;
    }
    e -= 1023;
    v.u = (v.u & 0x000fffffffffffffull) | 0x3ff0000000000000ull; /* m in [1, 2) */
    double m = v.d;
    if (m > 1.4142135623730951) {
        m *= 0.5;
        e++;
    }
    double s = (m - 1.0) / (m + 1.0), s2 = s * s, term = s, sum = 0;
    for (int k = 1; k < 40; k += 2) {
        sum += term / k;
        term *= s2;
    }
    return 2.0 * sum + e * 0.69314718055994530942;
}

/* libxmp checks that a path is a file */
int stat(const char *path, struct stat *st)
{
    rg_stat_t s = rg_storage_stat(path);
    if (!s.exists) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = s.is_dir ? S_IFDIR : S_IFREG;
    st->st_size = (off_t)s.size;
    return 0;
}
