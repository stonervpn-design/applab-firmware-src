#include "fg_ui.h"
#include "fg_progress.h"
#include "fg_geo.h"
#include <string.h>
#include <stdio.h>

static const char RENAME_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
#define RENAME_NCHARS (sizeof(RENAME_CHARS) - 1)

const char* fg_ui_face(FgApp* app, uint32_t now, const char** mood) {
    uint32_t hz = configTICK_RATE_HZ;
    uint32_t idle = now - app->last_pwn_tick;
    bool friend_near = app->peers.count && (now - app->last_friend_tick) < 8 * hz;

    if(idle < 3 * hz) {
        *mood = "PWND!";
        return "(^o^)";
    }
    if(friend_near) {
        *mood = "FRIEND";
        return "(^3^)";
    }
    if(app->attack_on && app->nets.count > 0 && idle < 15 * hz) {
        *mood = "hunting";
        return "(-_o)";
    }
    if(idle < 8 * hz) {
        *mood = "happy";
        return "(^_^)";
    }
    if(idle < 30 * hz) {
        *mood = "looking";
        return ((now / hz) & 1) ? "( o_o)" : "(o_o )";
    }
    if(app->nets.count == 0 && idle > 30 * hz) {
        *mood = "no nets";
        return "(T_T)";
    }
    if(idle < 60 * hz) {
        *mood = "bored";
        return "(-_-)";
    }
    if(idle < 120 * hz) {
        *mood = "lonely";
        return "(._.)";
    }
    *mood = "zZz";
    return "(u_u)";
}

static void draw_face_screen(Canvas* canvas, FgApp* app) {
    char buf[48];
    uint32_t now = furi_get_tick();
    uint32_t up = (now - app->start_tick) / configTICK_RATE_HZ;
    uint32_t pwn = app->prog.total_pwns;
    uint32_t level = fg_level_of(app->prog.xp);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, app->prog.name[0] ? app->prog.name : "Flippergotchi");
    snprintf(buf, sizeof(buf), "%sCH%u", fg_geo_has_fix() ? "GPS " : "", app->channel);
    canvas_draw_str_aligned(canvas, 126, 8, AlignRight, AlignTop, buf);

    const char* mood = "";
    const char* face = fg_ui_face(app, now, &mood);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignTop, face);
    canvas_set_font(canvas, FontSecondary);
    snprintf(
        buf, sizeof(buf), "Lv%lu %s %lu:%02lu", (unsigned long)level, mood,
        (unsigned long)(up / 60), (unsigned long)(up % 60));
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignTop, buf);

    snprintf(
        buf, sizeof(buf), "PWN%lu HS%lu KID%lu", (unsigned long)pwn,
        (unsigned long)app->handshakes, (unsigned long)app->pmkids);
    canvas_draw_str(canvas, 2, 54, buf);
    snprintf(
        buf, sizeof(buf), "AP%u CL%u FR%u %s", app->nets.count, app->client_count,
        app->peers.count, app->attack_on ? (app->solicit_on ? "ATK+" : "ATK") : "pas");
    canvas_draw_str(canvas, 2, 63, buf);
}

static void draw_list_header(Canvas* canvas, const char* title) {
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 8, title);
    canvas_draw_line(canvas, 0, 10, 127, 10);
}

static void draw_nets_screen(Canvas* canvas, FgApp* app) {
    char buf[40];
    uint16_t n = app->nets.count;
    snprintf(buf, sizeof(buf), "Nets %u (pwn %u)", n, fg_net_pwned_count(&app->nets));
    draw_list_header(canvas, buf);
    if(app->list_sel >= n && n) app->list_sel = n - 1;
    // window of 5 rows around list_sel
    uint16_t top = 0;
    if(app->list_sel >= 5) top = app->list_sel - 4;
    for(uint16_t r = 0; r < 5 && (top + r) < n; r++) {
        uint16_t i = top + r;
        FgNet* net = &app->nets.nets[i];
        char flags[4] = "   ";
        if(net->flags & FG_NET_HAS_PMKID) flags[0] = 'P';
        if(net->flags & FG_NET_HAS_M2) flags[1] = 'H';
        if(net->flags & FG_NET_FULL) flags[2] = 'F';
        snprintf(
            buf, sizeof(buf), "%c%.14s C%u %s", (i == app->list_sel) ? '>' : ' ',
            net->ssid[0] ? net->ssid : "(hidden)", net->channel, flags);
        canvas_draw_str(canvas, 2, 20 + r * 9, buf);
    }
    if(!n) canvas_draw_str(canvas, 2, 30, "scanning...");
}

static void draw_peers_screen(Canvas* canvas, FgApp* app) {
    char buf[40];
    uint8_t n = app->peers.count;
    snprintf(buf, sizeof(buf), "Peers %u (fr %lu)", n, (unsigned long)app->friends_total);
    draw_list_header(canvas, buf);
    if(app->list_sel >= n && n) app->list_sel = n - 1;
    uint16_t top = 0;
    if(app->list_sel >= 5) top = app->list_sel - 4;
    for(uint16_t r = 0; r < 5 && (top + r) < n; r++) {
        uint16_t i = top + r;
        FgPeer* p = &app->peers.peers[i];
        uint32_t age = (furi_get_tick() - p->last_seen) / configTICK_RATE_HZ;
        snprintf(
            buf, sizeof(buf), "%c%.12s p%lu %lus", (i == app->list_sel) ? '>' : ' ', p->name,
            (unsigned long)p->pwnd, (unsigned long)age);
        canvas_draw_str(canvas, 2, 20 + r * 9, buf);
    }
    if(!n) canvas_draw_str(canvas, 2, 30, "no friends yet");
}

static void draw_stats_screen(Canvas* canvas, FgApp* app) {
    char buf[40];
    draw_list_header(canvas, "Stats");
    uint32_t xp = app->prog.xp;
    uint32_t lv = fg_level_of(xp);
    uint32_t base = fg_xp_for_level(lv), next = fg_xp_for_level(lv + 1);
    uint32_t span = next > base ? next - base : 1;
    int barw = (int)((xp - base) * 100 / span);
    snprintf(buf, sizeof(buf), "Lv%lu  %lu/%lu XP", (unsigned long)lv, (unsigned long)xp,
             (unsigned long)next);
    canvas_draw_str(canvas, 2, 20, buf);
    canvas_draw_frame(canvas, 2, 23, 102, 6);
    canvas_draw_box(canvas, 3, 24, barw > 100 ? 100 : barw, 4);

    snprintf(
        buf, sizeof(buf), "streak %lu best %lu", (unsigned long)app->prog.cur_streak,
        (unsigned long)app->prog.best_streak);
    canvas_draw_str(canvas, 2, 38, buf);

    // achievement dots row
    canvas_draw_str(canvas, 2, 48, "Ach:");
    for(int i = 0; i < FG_ACH_COUNT; i++) {
        int x = 26 + i * 12;
        if(app->prog.ach & (1u << i))
            canvas_draw_box(canvas, x, 42, 8, 6);
        else
            canvas_draw_frame(canvas, x, 42, 8, 6);
    }

    if(app->renaming) {
        snprintf(buf, sizeof(buf), "Name: %s", app->rename_buf);
        canvas_draw_str(canvas, 2, 62, buf);
        // cursor underline
        int cx = 2 + canvas_string_width(canvas, "Name: ") + app->rename_pos * 6;
        canvas_draw_line(canvas, cx, 63, cx + 5, 63);
    } else {
        canvas_draw_str(canvas, 2, 62, "OK: rename pet");
    }
}

void fg_ui_draw(Canvas* canvas, void* model) {
    FgApp* app = *(FgApp**)model;
    canvas_clear(canvas);
    switch(app->screen) {
    case FG_SCR_NETS: draw_nets_screen(canvas, app); break;
    case FG_SCR_PEERS: draw_peers_screen(canvas, app); break;
    case FG_SCR_STATS: draw_stats_screen(canvas, app); break;
    case FG_SCR_FACE:
    default: draw_face_screen(canvas, app); break;
    }
}

static uint8_t char_index(char c) {
    for(uint8_t i = 0; i < RENAME_NCHARS; i++)
        if(RENAME_CHARS[i] == c) return i;
    return 0;
}

bool fg_ui_input(InputEvent* event, void* ctx) {
    FgApp* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    // ---- rename modal (on STATS) ----
    if(app->renaming) {
        switch(event->key) {
        case InputKeyUp: {
            uint8_t ci = char_index(app->rename_buf[app->rename_pos]);
            app->rename_buf[app->rename_pos] = RENAME_CHARS[(ci + 1) % RENAME_NCHARS];
            return true;
        }
        case InputKeyDown: {
            uint8_t ci = char_index(app->rename_buf[app->rename_pos]);
            app->rename_buf[app->rename_pos] = RENAME_CHARS[(ci + RENAME_NCHARS - 1) % RENAME_NCHARS];
            return true;
        }
        case InputKeyLeft:
            if(app->rename_pos) app->rename_pos--;
            return true;
        case InputKeyRight:
            if(app->rename_pos < 11 && app->rename_buf[app->rename_pos]) {
                app->rename_pos++;
                if(!app->rename_buf[app->rename_pos]) app->rename_buf[app->rename_pos] = 'A';
            }
            return true;
        case InputKeyOk: {
            // trim trailing 'A'-padding is not needed; copy as-is (min 2 chars)
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            strncpy(app->prog.name, app->rename_buf, sizeof(app->prog.name) - 1);
            app->prog.name[sizeof(app->prog.name) - 1] = '\0';
            furi_mutex_release(app->mutex);
            fg_progress_save(app->storage, &app->prog);
            app->renaming = false;
            return true;
        }
        case InputKeyBack:
            app->renaming = false;
            return true;
        default: return false;
        }
    }

    // ---- normal navigation ----
    switch(event->key) {
    case InputKeyBack:
        view_dispatcher_stop(app->view_dispatcher);
        return true;
    case InputKeyLeft:
        app->screen = (app->screen + FG_SCR_COUNT - 1) % FG_SCR_COUNT;
        app->list_sel = 0;
        return true;
    case InputKeyRight:
        app->screen = (app->screen + 1) % FG_SCR_COUNT;
        app->list_sel = 0;
        return true;
    case InputKeyUp:
        if(app->screen == FG_SCR_NETS || app->screen == FG_SCR_PEERS) {
            if(app->list_sel) app->list_sel--;
        } else if(app->screen == FG_SCR_FACE) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->solicit_on = !app->solicit_on;
            furi_mutex_release(app->mutex);
        }
        return true;
    case InputKeyDown:
        if(app->screen == FG_SCR_NETS && app->list_sel + 1 < app->nets.count)
            app->list_sel++;
        else if(app->screen == FG_SCR_PEERS && app->list_sel + 1 < app->peers.count)
            app->list_sel++;
        return true;
    case InputKeyOk:
        if(app->screen == FG_SCR_FACE) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            app->attack_on = !app->attack_on;
            furi_mutex_release(app->mutex);
        } else if(app->screen == FG_SCR_STATS) {
            // start rename from current name (padded)
            memset(app->rename_buf, 0, sizeof(app->rename_buf));
            strncpy(app->rename_buf, app->prog.name, sizeof(app->rename_buf) - 1);
            if(!app->rename_buf[0]) strcpy(app->rename_buf, "AA");
            app->rename_pos = 0;
            app->renaming = true;
        }
        return true;
    default: return false;
    }
}
