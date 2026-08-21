#pragma once
/* Host-test shim for lib/subghz/protocols/base.h.
 *
 * keeloq_common.h and aes_common.h each do `#include "base.h"` (quote-form, so
 * gcc resolves the file living NEXT TO them -- a same-directory quoted include
 * cannot be overridden by -I). base.h's REAL content only declares the
 * SubGhzProtocolDecoderBase/EncoderBase plumbing via ../types.h, which drags in
 * the whole furi_hal/flipper_format stack. The pure crypto in keeloq_common.c
 * and aes_common.c uses NONE of that (verified: only uintNN_t). So the Makefile
 * stages byte-identical COPIES of the real *.{c,h} into stage/ and drops this
 * empty stub in as their base.h, letting the real crypto compile untouched.
 */
