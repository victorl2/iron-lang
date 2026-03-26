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
#include "rshapes.c"
#include "rtextures.c"
#include "rtext.c"
#include "rmodels.c"
#include "raudio.c"
#include "utils.c"
