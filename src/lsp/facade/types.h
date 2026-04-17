#ifndef IRON_LSP_FACADE_TYPES_H
#define IRON_LSP_FACADE_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* LSP facade-layer type declarations. Phase 2 Plan 01 ships the minimum
 * shape every later plan (hover/go-to-def/diagnostics) needs: Position,
 * Range, and the negotiated position encoding. Iron_Diagnostic etc. are
 * forward-declared here and fleshed out as plans land. */

typedef struct IronLsp_Position {
    uint32_t line;       /* 0-based line number */
    uint32_t character;  /* 0-based char offset in the negotiated encoding */
} IronLsp_Position;

typedef struct IronLsp_Range {
    IronLsp_Position start;
    IronLsp_Position end;
} IronLsp_Range;

typedef enum IronLsp_PositionEncoding {
    ILSP_ENC_UTF8 = 0,
    ILSP_ENC_UTF16
} IronLsp_PositionEncoding;

struct IronLsp_Diagnostic;

#endif /* IRON_LSP_FACADE_TYPES_H */
