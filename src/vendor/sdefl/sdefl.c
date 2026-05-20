/* Compilation unit for the vendored sdefl single-header DEFLATE encoder.
 *
 * Upstream `sdefl.h` ships as a stb-style single-header library — the
 * declarations are visible to any TU that includes the header, and the
 * implementation appears only when the consumer defines the impl macro
 * before the include. pakka's vendor convention splits each codec into
 * a .h that the rest of the tree consumes and a .c that compiles the
 * impl exactly once. This file is that .c.
 *
 * Identifier rename to `pakka_sdefl` / `PAKKA_SDEFL` was applied at
 * vendor time so `make symbol-audit` sees only `pakka_`-prefixed
 * defined globals. See src/vendor/sdefl/VENDOR.md.
 */

/* MSVC: _BitScanReverse intrinsic needs <intrin.h> for a real
 * declaration. Without it, the upstream `pakka_sdefl_ilog2` call
 * site relies on implicit-decl and trips /W4 /WX. Include is a
 * no-op on POSIX compilers. */
#ifdef _MSC_VER
#include <intrin.h>
#endif

#define PAKKA_SDEFL_IMPLEMENTATION
#include "sdefl.h"
