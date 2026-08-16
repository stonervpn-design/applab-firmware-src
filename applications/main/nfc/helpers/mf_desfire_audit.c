// mf_desfire_audit.c
#include "mf_desfire_audit.h"

bool mf_desfire_audit_is_free_read(MfDesfireFileAccessRights ar) {
    // Read = bits[15:12], ReadWrite = bits[7:4]; 0xE = free (no key
    // needed) on either nibble grants unauthenticated read, matching
    // mf_desfire_render.c's read_rights == 0x0e || read_write_rights ==
    // 0x0e. See mf_desfire_audit.h for the full nibble layout and the
    // render.c cross-check.
    return (((ar >> 12) & 0x0F) == 0x0E) || (((ar >> 4) & 0x0F) == 0x0E);
}

#ifndef DESFIRE_HOST_TEST

void mf_desfire_audit_file(const MfDesfireFileSettings* fs, FuriString* report_out) {
    const char* comm_str;
    switch(fs->comm) {
    case MfDesfireFileCommunicationSettingsPlaintext:
        comm_str = "plaintext (no encryption/integrity)";
        break;
    case MfDesfireFileCommunicationSettingsAuthenticated:
        comm_str = "authenticated (MACed, not encrypted)";
        break;
    case MfDesfireFileCommunicationSettingsEnciphered:
        comm_str = "enciphered";
        break;
    default:
        comm_str = "unknown";
        break;
    }
    furi_string_cat_printf(report_out, "Comm mode: %s\n", comm_str);

    bool any_free_read = false;
    for(uint8_t i = 0; i < fs->access_rights_len; i++) {
        if(mf_desfire_audit_is_free_read(fs->access_rights[i])) {
            any_free_read = true;
            furi_string_cat_printf(
                report_out,
                "WEAK: access-rights slot %d allows FREE READ (no authentication required)\n",
                i);
        }
    }
    if(!any_free_read) {
        furi_string_cat_printf(report_out, "OK: no free-read access-rights slot found\n");
    }

    if(any_free_read && fs->comm == MfDesfireFileCommunicationSettingsPlaintext) {
        furi_string_cat_printf(
            report_out,
            "WEAK: free-read file also uses plaintext comm mode "
            "(contents readable to any nearby reader)\n");
    }
}

void mf_desfire_audit_key_settings(
    const MfDesfireKeySettings* ks, bool default_master_found, FuriString* report_out) {
    if(default_master_found) {
        furi_string_cat_printf(
            report_out, "WEAK: default (factory) master/application key is still active\n");
    } else {
        furi_string_cat_printf(
            report_out, "OK: default master/application key not found (not tried, or rejected)\n");
    }

    if(ks->is_free_directory_list) {
        furi_string_cat_printf(
            report_out, "WEAK: application/file directory listing requires no authentication\n");
    }
    if(ks->is_free_create_delete) {
        furi_string_cat_printf(
            report_out, "WEAK: applications/files can be created or deleted without authentication\n");
    }

    furi_string_cat_printf(
        report_out,
        "NOTE: master key is %schangeable\n",
        ks->is_master_key_changeable ? "" : "NOT ");
    furi_string_cat_printf(
        report_out,
        "NOTE: PICC/application configuration is %s\n",
        ks->is_config_changeable ? "changeable" : "locked");

    furi_string_cat_printf(report_out, "changeKeyID %d, maxKeys %d\n", ks->change_key_id, ks->max_keys);
}

#endif // DESFIRE_HOST_TEST
