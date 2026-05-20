/* Compilation unit for the vendored sinfl single-header DEFLATE decoder.
 *
 * Pairs with src/vendor/sinfl/sinfl.h. Implementation is gated behind
 * PAKKA_SINFL_IMPLEMENTATION (renamed from upstream's SINFL_IMPLEMENTATION
 * at vendor time).
 *
 * PAKKA_SINFL_NO_SIMD is forced on: upstream's NEON / SSE2 fast paths
 * use intrinsics that pakka's CI matrix (MSVC arm64, big-endian s390x
 * under QEMU, legacy NetBSD/sparc with gcc 3.x) doesn't have the
 * conditional-include glue for. The portable byte-loop path is fast
 * enough for pakka's "extract on user invocation" workflow — peak
 * inflate is ~5 MiB largest Q3 demo asset, well under a second.
 */

/* MSVC: _BitScanReverse needs <intrin.h> for the real declaration —
 * same rationale as src/vendor/sdefl/sdefl.c. No-op on POSIX. */
#ifdef _MSC_VER
#include <intrin.h>
#endif

#define PAKKA_SINFL_NO_SIMD
#define PAKKA_SINFL_IMPLEMENTATION
#include "sinfl.h"
