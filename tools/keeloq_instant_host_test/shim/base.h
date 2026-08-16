#pragma once
/* Host-test shim for lib/subghz/protocols/base.h.
 *
 * keeloq_common.h does `#include "base.h"` (quote-form, so gcc always finds
 * the file living NEXT TO keeloq_common.h -- -I search paths cannot
 * override a quoted include when a same-named file sits in the including
 * file's own directory). base.h's REAL content just declares
 * SubGhzProtocolDecoderBase / SubGhzProtocolEncoderBase plumbing via
 * ../types.h, which in turn pulls in the full furi_hal / flipper_format
 * stack -- verified (by reading keeloq_common.h) that keeloq_common.h uses
 * NONE of that; it only needs uint32_t/uint64_t. So the Makefile stages a
 * byte-identical COPY of the real keeloq_common.{c,h} into stage/ and drops
 * this empty stub in next to them as their base.h, letting the real crypto
 * source compile untouched with no furi dependency at all.
 */
