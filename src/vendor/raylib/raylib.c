/* raylib.c — Single-translation-unit build for raylib 5.5
 *
 * This file is an amalgamation driver: it includes all raylib source modules
 * so the Iron build pipeline can compile raylib by referencing a single .c
 * file (vendor/raylib/raylib.c).
 *
 * The -I vendor/raylib flag (set by build.c) ensures all internal #includes
 * within these files resolve correctly against files in this directory.
 *
 * PLATFORM_DESKTOP must be defined by the caller (-DPLATFORM_DESKTOP).
 * The Iron build pipeline sets this flag in invoke_clang (see src/cli/build.c).
 */

#include "rcore.c"

/* rcore.c #defines RLGL_IMPLEMENTATION before including rlgl.h, which emits
 * the full ~4400-line implementation block. rlgl.h's header guard (#ifndef
 * RLGL_H) only wraps the *declarations*; the `#if defined(RLGL_IMPLEMENTATION)`
 * block sits outside the guard. So when rshapes.c / rtextures.c / rtext.c /
 * rmodels.c re-include rlgl.h in this same translation unit, the declarations
 * are correctly skipped — but the implementation block re-emits, producing
 * "redefinition of 'rlglData'" and hundreds more errors.
 *
 * Undef RLGL_IMPLEMENTATION here so subsequent re-includes pick up only the
 * declarations from the header guard, not the implementation. */
#undef RLGL_IMPLEMENTATION

#include "rshapes.c"
#include "rtextures.c"
#include "rtext.c"
#include "rmodels.c"
#include "raudio.c"
#include "utils.c"
