// mf_desfire_audit.h
//
// DESFire config-weakness audit: PASSIVE analysis of already-read file
// settings / key settings for common misconfigurations (free-read files,
// weak comm mode, default (factory) master key still active, changeable
// config flags). This module never talks to the card — it only formats
// findings from data the poller/attack-sweep already collected.
//
// mf_desfire_audit_is_free_read is pure logic: it takes a raw 16-bit
// access-rights word and returns a bool, with no firmware dependency. That
// is what lets it host-test unchanged in tools/desfire_auth_host_test
// (compiled with -DDESFIRE_HOST_TEST). The report builders below it need
// the real MfDesfireFileSettings / MfDesfireKeySettings structs and
// FuriString, which are firmware-only, so they (and their firmware-only
// includes) are compiled out of the host build.
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef DESFIRE_HOST_TEST
// Mirrors the real typedef in
// components/nfc/protocols/mf_desfire/mf_desfire.h
// (`typedef uint16_t MfDesfireFileAccessRights;`) without pulling in that
// header's firmware-only includes (protocols/iso14443_4a/iso14443_4a.h,
// toolbox/simple_array.h), which don't compile on host.
typedef uint16_t MfDesfireFileAccessRights;
#else
#include <furi_string.h>
#include <nfc/protocols/mf_desfire/mf_desfire.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Per DESFire spec, a file's 16-bit access-rights word packs four 4-bit
// fields: bits[15:12]=Read, [11:8]=Write, [7:4]=ReadWrite, [3:0]=Change. A
// nibble value of 0xE means "free" (no authentication required); 0xF means
// "never" (denied to every key). This matches the interpretation already
// used by the in-tree renderer,
// applications/main/nfc/helpers/protocol_support/mf_desfire/mf_desfire_render.c
// (`settings->access_rights[i] >> 12 & 0xF` for Read, `>> 4 & 0xF` for
// ReadWrite, etc. — see nfc_render_mf_desfire_file_settings_data).
//
// Returns true if the READ nibble (bits[15:12]) of `ar` is 0xE, i.e. the
// file can be read without authenticating.
bool mf_desfire_audit_is_free_read(MfDesfireFileAccessRights ar);

#ifndef DESFIRE_HOST_TEST
// Appends human-readable config-weakness findings for a single file's
// settings to report_out: comm mode (plaintext/authenticated/enciphered)
// and, for every access-rights slot on the file, whether it grants free
// (unauthenticated) read.
void mf_desfire_audit_file(const MfDesfireFileSettings* fs, FuriString* report_out);

// Appends human-readable config-weakness findings for a PICC master's or
// application's key settings to report_out: whether the default (factory)
// key was found still active (as determined by the caller's dictionary
// sweep and passed in via default_master_found), and which config flags
// remain changeable (master key changeable, config changeable, free
// create/delete, free directory listing).
void mf_desfire_audit_key_settings(
    const MfDesfireKeySettings* ks, bool default_master_found, FuriString* report_out);
#endif

#ifdef __cplusplus
}
#endif
