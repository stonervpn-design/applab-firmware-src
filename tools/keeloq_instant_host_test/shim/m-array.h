#pragma once
/* Host-test shim for <m-array.h>. keeloq_common.c #includes it but (verified
 * by grep) never invokes any M*ARRAY_DEF/M_EACH macro -- nothing in the
 * crypto path uses mlib containers. Empty stub is sufficient. */
