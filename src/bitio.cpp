#include "bitio.h"

// Everything in BitWriter/BitReader is on the per-symbol hot path and lives inline in
// the header. This translation unit exists so the header is compiled on its own at least
// once, which catches missing includes that would otherwise only surface for a consumer.
