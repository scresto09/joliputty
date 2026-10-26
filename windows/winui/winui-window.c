/*
 * winui-window.c - the PuTTY main program for the WinUI 3 front end.
 *
 * This is the C half of the front end: it owns the sessions (one
 * WinGuiSeat per tab), and implements the Seat and TermWin
 * interfaces, keyboard translation, mouse handling, menus, the
 * clipboard and the session management commands. Everything to do
 * with the actual XAML window, and with rendering, is done in
 * winui-host.cpp, via the interface in wui.h.
 *
 * Much of this is derived from windows/window.c, which is still used
 * by PuTTYtel and pterm.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include <wchar.h>

#include "putty.h"
#include "ssh.h"
#include "terminal.h"
#include "storage.h"
#include "putty-rc.h"
#include "security-api.h"
#include "win-gui-seat.h"
#include "tree234.h"

#include <imm.h>
#include <commctrl.h>
#include <richedit.h>
#include <mmsystem.h>
#include <shobjidl.h>

#include "wui.h"
#include "wuidlg.h"
#include "winui-sessions.h"
#include "winui-config.h"
#include "autologon.h"

/* From MSDN: In the WM_SYSCOMMAND message, the four low-order bits of
 * wParam are used by Windows, and should be masked off, so we shouldn't
 * attempt to store information in them. Hence all these identifiers have
 * the low 4 bits clear. Also, identifiers should < 0xF000. */

#define IDM_SHOWLOG   0x0010
#define IDM_NEWSESS   0x0020
#define IDM_NEWWIN    0x0080
#define IDM_DUPSESS   0x0030
#define IDM_RESTART   0x0040
#define IDM_RECONF    0x0050
#define IDM_CLRSB     0x0060
#define IDM_RESET     0x0070
#define IDM_ABOUT     0x0150
#define IDM_SAVEDSESS 0x0160
#define IDM_COPYALL   0x0170
#define IDM_FULLSCREEN  0x0180
#define IDM_COPY      0x0190
#define IDM_PASTE     0x01A0
#define IDM_TABLEFT   0x01B0
#define IDM_TABRIGHT  0x01C0
#define IDM_CLOSETAB  0x01D0
#define IDM_FIND      0x01E0
#define IDM_SENDFILE  0x01F0
#define IDM_RECVFILE  0x0200
#define IDM_SPLITRIGHT 0x0210
#define IDM_SPLITDOWN 0x0220
#define IDM_CLOSEPANE 0x0230
#define IDM_QUICK     0x0240           /* see wui_cb_quick_connect */
#define IDM_RENAMETAB 0x0250
#define IDM_POOL_SESSION 0x0260        /* see show_pool */
#define IDM_POOL_COMMON  0x0270
#define IDM_POOL_PASTE   0x0280
#define IDM_HELP      0x0290           /* see wui_help_url */

/* This front end's version, and the release of PuTTY it's based on, from
 * the git tags (see cmake/joliputty-version.cmake; and show_about, and
 * WinMain, for what it tells SSH servers it is) */
#include "joliputty-version.h"
/* ... and where its manual is, on its web site: a copy for each
 * version, so that it's always the one for this one (and the latest
 * release's, in latest/, for a build that isn't a release: see
 * .github/workflows/help.yml) */
#if JOLIPUTTY_RELEASE
#define JOLIPUTTY_HELP "https://scresto09.github.io/joliputty/" \
    JOLIPUTTY_VERSION "/"
#else
#define JOLIPUTTY_HELP "https://scresto09.github.io/joliputty/latest/"
#endif

#define IDM_SPECIAL_MIN 0x0400
#define IDM_SPECIAL_MAX 0x0800

#define IDM_SAVED_MIN 0x1000
#define IDM_SAVED_MAX 0x5000
#define MENU_SAVED_STEP 16
/* Maximum number of sessions on saved-session submenu */
#define MENU_SAVED_MAX ((IDM_SAVED_MAX-IDM_SAVED_MIN) / MENU_SAVED_STEP)

/* Messages to our hidden message window */
#define WM_IGNORE_CLIP     (WM_APP + 2)
#define WM_GOT_CLIPDATA    (WM_APP + 4)
#define WM_WUI_CALLBACKS   (WM_APP + 20)
#define WM_WUI_HANDLEWAIT  (WM_APP + 21)
#define WM_WUI_COMMAND     (WM_APP + 22)
#define WM_WUI_CLOSE       (WM_APP + 23)
#define WM_WUI_CLOSE_TAB   (WM_APP + 24)
#define WM_WUI_CLOSE_PANE  (WM_APP + 26)
#define WM_WUI_LOOKUP      (WM_APP + 27)   /* see start_backend */

/* Needed for Chinese support and apparently not always defined. */
#ifndef VK_PROCESSKEY
#define VK_PROCESSKEY 0xE5
#endif

#ifndef WHEEL_DELTA
#define WHEEL_DELTA 120
#endif

/* VK_PACKET, used to send Unicode characters in WM_KEYDOWNs */
#ifndef VK_PACKET
#define VK_PACKET 0xE7
#endif

#define IS_HIGH_VARSEL(wch1, wch2) \
    ((wch1) == 0xDB40 && ((wch2) >= 0xDD00 && (wch2) <= 0xDDEF))
#define IS_LOW_VARSEL(wch) \
    (((wch) >= 0x180B && (wch) <= 0x180D) || /* MONGOLIAN FREE VARIATION SELECTOR */ \
     ((wch) >= 0xFE00 && (wch) <= 0xFE0F)) /* VARIATION SELECTOR 1-16 */

/*
 * One of these per session, i.e. per tab. The WinGuiSeat structure is
 * shared with dialog.c, so we keep the extra state that only this
 * front end needs in a wrapper around it.
 */
typedef struct WuiSeat {
    WinGuiSeat wgs;
    char *session_name;                /* the saved session it's from */

    WuiTerm *tterm;                    /* this session's tab */
    unsigned id;                       /* for finding it again later */
    unsigned lookup;                   /* the host name lookup it's
                                        * waiting for, if any */
    bool welcome;                      /* the welcome tab, with no
                                        * session in it (see
                                        * show_welcome) */
    bool terminal_focused;
    bool drawing;
    bool border_dirty;
    HICON trust_icon;
    WuiLog *log;                       /* its Event Log window, if open */

    /* The pools of text to paste again (see show_pool): this tab's,
     * newest first; where the next copy is to be kept as well, if
     * anywhere (a WUI_POOL_*, by Copy to Pool); and the pools as the
     * pop-up last showed them, for what's done in it */
    wchar_t **pool;
    size_t npool, poolsize;
    int pool_target;
    bool pool_skip;                    /* (for Copy All: see pool_copied) */
    WuiPoolItem *shown;
    size_t nshown;
    bool unzoom_pending;               /* (see handle_size) */

    /* Automate logon (see autologon_start) */
    AutologonStep *al_steps;
    size_t al_nsteps;
    size_t al_step;                    /* the one we're waiting for */
    size_t al_sent;                    /* (steps before it not yet sent) */
    strbuf *al_seen;                   /* output since the last step */
    char *al_echo;                     /* a hidden command, whose echo... */
    size_t al_echo_pos;
    strbuf *al_held;                   /* ... this much of has arrived */

    /* Find (see wui_cb_search) */
    bool found;                        /* a match is selected... */
    int found_y, found_x;              /* ... starting here */

    /* Send File and Receive File (see xfer_bar_start) */
    bool bar_receive;                  /* which the bar's for */
    SftpXfer *xfer;                    /* the bar's job... */
    int xfer_kind;                     /* ... which is an XferKind */
    SftpXfer *list_xfer;               /* listing a remote directory... */
    wchar_t *list_dir;                 /* ... this one... */
    wchar_t **list_names;              /* ... and what's in it */
    size_t n_list, list_size;
    Filename *send_file;               /* the file being sent */
    char *send_dir;                    /* where the last one went */
    char *recv_path;                   /* the file being received */
    bool recv_asking;                  /* (asking where to put it) */
    char *recv_dir;                    /* where the last one came from */
    wchar_t *recv_local, *recv_part;   /* where to, and meanwhile */
} WuiSeat;

#define WUI(wgs) container_of(wgs, WuiSeat, wgs)

/* All the sessions are on wgslisthead; this one is in the visible tab */
static WuiSeat *active;
static unsigned seat_serial;
static int nseats;

/* The window, and its state */
static WuiHost *host;
static int show_cmd;
static WuiWindowState win_state;
static WNDPROC old_wndproc;
static Conf *initial_conf;             /* from the command line */
static char *initial_session;          /* ... and the session it named */
static bool startup_config;            /* ... which didn't say what to
                                        * connect to */

/* The menu last built by build_menu */
static WuiMenuItem *menu;
static size_t menu_len, menu_size;
static wchar_t **menu_strings;
static size_t n_menu_strings, menu_strings_size;

static HWND msg_hwnd;                  /* hidden window for messages */
static HHOOK key_hook;
static int exit_code;
static bool ignore_clip;
static unsigned clip_owner;            /* id of the seat that last copied */

static Mouse_Button translate_button(WinGuiSeat *wgs, Mouse_Button button);
static void show_mouseptr(WinGuiSeat *wgs, bool show);
static int TranslateKey(WinGuiSeat *wgs, UINT message, WPARAM wParam,
                        LPARAM lParam, unsigned char *output);
static void init_palette(WinGuiSeat *wgs);
static void init_fonts(WinGuiSeat *wgs, int, int);
static void deinit_fonts(WinGuiSeat *wgs);
static void set_input_locale(WinGuiSeat *wgs, HKL);
static void init_winfuncs(void);
static void do_command(WinGuiSeat *wgs, unsigned id);
static void set_session_name(WuiSeat *ws, char *name);
static void update_tab_label(WuiSeat *ws);
static void split_session(WuiSeat *ws, bool side_by_side);
static void update_mouse_pointer(WinGuiSeat *wgs);

static bool is_full_screen(WinGuiSeat *wgs);
static bool is_zoomed(WinGuiSeat *wgs);
static void make_full_screen(WinGuiSeat *wgs);
static void clear_full_screen(WinGuiSeat *wgs);
static void flip_full_screen(WinGuiSeat *wgs);
static void process_clipdata(WinGuiSeat *wgs, HGLOBAL clipdata, bool unicode);
static void setup_clipboards(Terminal *, Conf *);
static void update_scrollbar_visibility(WinGuiSeat *wgs);

/* Window layout information */
static void reset_window(WinGuiSeat *wgs, int reinit);
static void recompute_window_offset(WinGuiSeat *wgs);
static void invalidate_all(WinGuiSeat *wgs);

static void flash_window(WinGuiSeat *wgs, int mode);
static void sys_cursor_update(WinGuiSeat *wgs);
static bool get_fullscreen_rect(WinGuiSeat *wgs, RECT *ss);
static bool get_workingarea_rect(WinGuiSeat *wgs, RECT *ss);

static void conf_cache_data(WinGuiSeat *wgs);

static void frontend_busy_enter(void);
static void frontend_busy_leave(void);

static WuiSeat *open_session(Conf *conf);
static void send_file(WuiSeat *ws);
static void receive_file(WuiSeat *ws);
static void xfer_stop(WuiSeat *ws);
static void xfer_free(WuiSeat *ws);
static void launch_first_session(bool welcome);
static void close_tab(WinGuiSeat *wgs);
static void queue_close_tab(WinGuiSeat *wgs);
static void handle_size(WinGuiSeat *wgs, int width, int height);

static struct sesslist sesslist;       /* for saved-session menu */

enum MONITOR_DPI_TYPE { MDT_EFFECTIVE_DPI, MDT_ANGULAR_DPI, MDT_RAW_DPI, MDT_DEFAULT };
DECL_WINDOWS_FUNCTION(static, BOOL, GetMonitorInfoA, (HMONITOR, LPMONITORINFO));
DECL_WINDOWS_FUNCTION(static, HMONITOR, MonitorFromPoint, (POINT, DWORD));
DECL_WINDOWS_FUNCTION(static, HMONITOR, MonitorFromWindow, (HWND, DWORD));

struct WinGuiSeatListNode wgslisthead = {
    .next = &wgslisthead, .prev = &wgslisthead,
};

static bool wintw_setup_draw_ctx(TermWin *);
static void wintw_draw_text(TermWin *, int x, int y, wchar_t *text, int len,
                            unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_cursor(TermWin *, int x, int y, wchar_t *text, int len,
                              unsigned long attrs, int lattrs, truecolour tc);
static void wintw_draw_trust_sigil(TermWin *, int x, int y);
static int wintw_char_width(TermWin *, int uc);
static void wintw_free_draw_ctx(TermWin *);
static void wintw_set_cursor_pos(TermWin *, int x, int y);
static void wintw_set_raw_mouse_mode(TermWin *, bool enable);
static void wintw_set_raw_mouse_mode_pointer(TermWin *, bool enable);
static void wintw_set_scrollbar(TermWin *, int total, int start, int page);
static void wintw_bell(TermWin *, int mode);
static void wintw_clip_write(
    TermWin *, int clipboard, wchar_t *text, int *attrs,
    truecolour *colours, int len, bool must_deselect);
static void wintw_clip_request_paste(TermWin *, int clipboard);
static void wintw_refresh(TermWin *);
static void wintw_request_resize(TermWin *, int w, int h);
static void wintw_set_title(TermWin *, const char *title, int codepage);
static void wintw_set_icon_title(TermWin *, const char *icontitle,
                                 int codepage);
static void wintw_set_minimised(TermWin *, bool minimised);
static void wintw_set_maximised(TermWin *, bool maximised);
static void wintw_move(TermWin *, int x, int y);
static void wintw_set_zorder(TermWin *, bool top);
static void wintw_palette_set(TermWin *, unsigned, unsigned, const rgb *);
static void wintw_palette_get_overrides(TermWin *, Terminal *);
static void wintw_unthrottle(TermWin *win, size_t bufsize);

static const TermWinVtable winui_termwin_vt = {
    .setup_draw_ctx = wintw_setup_draw_ctx,
    .draw_text = wintw_draw_text,
    .draw_cursor = wintw_draw_cursor,
    .draw_trust_sigil = wintw_draw_trust_sigil,
    .char_width = wintw_char_width,
    .free_draw_ctx = wintw_free_draw_ctx,
    .set_cursor_pos = wintw_set_cursor_pos,
    .set_raw_mouse_mode = wintw_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = wintw_set_raw_mouse_mode_pointer,
    .set_scrollbar = wintw_set_scrollbar,
    .bell = wintw_bell,
    .clip_write = wintw_clip_write,
    .clip_request_paste = wintw_clip_request_paste,
    .refresh = wintw_refresh,
    .request_resize = wintw_request_resize,
    .set_title = wintw_set_title,
    .set_icon_title = wintw_set_icon_title,
    .set_minimised = wintw_set_minimised,
    .set_maximised = wintw_set_maximised,
    .move = wintw_move,
    .set_zorder = wintw_set_zorder,
    .palette_set = wintw_palette_set,
    .palette_get_overrides = wintw_palette_get_overrides,
    .unthrottle = wintw_unthrottle,
};

const bool share_can_be_downstream = true;
const bool share_can_be_upstream = true;

#define FOR_EACH_SEAT(ws)                                               \
    for (struct WinGuiSeatListNode *ws##_node = wgslisthead.next,       \
             *ws##_next = ws##_node->next;                             \
         ws##_node != &wgslisthead &&                                   \
             ((ws) = container_of(container_of(ws##_node, WinGuiSeat,   \
                                               wgslistnode),            \
                                  WuiSeat, wgs), true);                 \
         ws##_node = ws##_next, ws##_next = ws##_node->next)

static WuiSeat *find_seat(unsigned id)
{
    WuiSeat *ws;
    FOR_EACH_SEAT(ws) {
        if (ws->id == id)
            return ws;
    }
    return NULL;
}

/* ----------------------------------------------------------------------
 * Automate logon (autologon.h), for SSH and Telnet: wait for each
 * step's text to arrive from the server, then send its command and
 * Enter, as if typed. A hidden command's echo, if the server sends
 * one, is kept off the screen. A step can also answer one of PuTTY's
 * own prompts (SSH's "login:", or a password prompt), whose text
 * is shown by the terminal rather than coming from the server.
 */

static void autologon_stop(WuiSeat *ws)
{
    autologon_free_steps(ws->al_steps, ws->al_nsteps);
    ws->al_steps = NULL;
    ws->al_nsteps = ws->al_step = ws->al_sent = 0;
    if (ws->al_seen) {
        strbuf_free(ws->al_seen);
        ws->al_seen = NULL;
    }
    sfree(ws->al_echo);
    ws->al_echo = NULL;
    ws->al_echo_pos = 0;
    if (ws->al_held) {
        strbuf_free(ws->al_held);
        ws->al_held = NULL;
    }
}

static void autologon_start(WuiSeat *ws)
{
    autologon_stop(ws);
    int protocol = conf_get_int(ws->wgs.conf, CONF_protocol);
    if (protocol != PROT_SSH && protocol != PROT_TELNET)
        return;
    ws->al_steps = autologon_get_steps(ws->wgs.conf, &ws->al_nsteps);
    /* (just the ones switched on, with a command to send: the default
     * steps have none until one is filled in) */
    size_t j = 0;
    for (size_t i = 0; i < ws->al_nsteps; i++) {
        AutologonStep *st = &ws->al_steps[i];
        if (st->active && st->stored) {
            /* (from the Credential Manager) */
            char *command = autologon_command(st);
            if (!command)
                logeventf(ws->wgs.logctx, "Automate logon: step %u's "
                          "command has gone from the Credential Manager",
                          (unsigned)i + 1);
            burnstr(st->command);
            st->command = command ? command : dupstr("");
            st->stored = false;
        }
        if (st->active && *st->command) {
            ws->al_steps[j++] = *st;
        } else {
            sfree(st->expect);
            burnstr(st->command);
        }
    }
    ws->al_nsteps = j;
    logeventf(ws->wgs.logctx, "Automate logon: %u step%s to do",
              (unsigned)j, j == 1 ? "" : "s");
    if (ws->al_nsteps) {
        ws->al_seen = strbuf_new();
        ws->al_held = strbuf_new();
    }
}

/* Send the steps that are due. (From a callback, not from inside
 * the output handler, since sending can echo output straight back.) */
static void autologon_send_callback(void *vctx)
{
    WuiSeat *ws = find_seat((unsigned)(uintptr_t)vctx);
    if (!ws)
        return;
    WinGuiSeat *wgs = &ws->wgs;
    while (ws->al_sent < ws->al_step) {
        AutologonStep *st = &ws->al_steps[ws->al_sent++];
        if (!wgs->ldisc)
            continue;
        logeventf(wgs->logctx, "Automate logon: sending command %u%s",
                  (unsigned)ws->al_sent, st->hide ? " (hidden)" : "");
        if (st->hide && *st->command) {
            sfree(ws->al_echo);
            ws->al_echo = dupstr(st->command);
            ws->al_echo_pos = 0;
            strbuf_clear(ws->al_held);
        }
        if (*st->command)
            ldisc_send(wgs->ldisc, st->command, strlen(st->command), false);
        ldisc_send(wgs->ldisc, "\r", -1, false);   /* the Return key */
    }
}

/* Move on past a matched step, and any after it with nothing to wait
 * for; returns whether there's anything to send */
static bool autologon_advance(WuiSeat *ws, bool matched)
{
    bool any = matched;
    if (matched) {
        ws->al_step++;
        strbuf_clear(ws->al_seen);
    }
    while (ws->al_step < ws->al_nsteps && !*ws->al_steps[ws->al_step].expect) {
        ws->al_step++;
        any = true;
    }
    return any;
}

static bool autologon_busy(WuiSeat *ws)
{
    return ws->al_step < ws->al_nsteps || ws->al_echo;
}

/* Output from the server, while there's something to do: look for
 * the text we're waiting for, and leave out a hidden echo */
static size_t autologon_output(WuiSeat *ws, const char *data, size_t len)
{
    strbuf *out = strbuf_new();
    bool send = (len > 0 && autologon_advance(ws, false));

    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        bool show = true;

        if (ws->al_echo) {
            if (c == ws->al_echo[ws->al_echo_pos]) {
                show = false;
                put_byte(ws->al_held, c);
                if (!ws->al_echo[++ws->al_echo_pos]) {
                    /* The whole of it: gone */
                    sfree(ws->al_echo);
                    ws->al_echo = NULL;
                    strbuf_clear(ws->al_held);
                }
            } else {
                /* Not an echo after all, so show what we held back */
                put_data(out, ws->al_held->s, ws->al_held->len);
                strbuf_clear(ws->al_held);
                sfree(ws->al_echo);
                ws->al_echo = NULL;
            }
        }
        if (show)
            put_byte(out, c);

        if (ws->al_step < ws->al_nsteps) {
            const char *e = ws->al_steps[ws->al_step].expect;
            size_t elen = strlen(e);
            strbuf *seen = ws->al_seen;
            put_byte(seen, c);
            if (seen->len >= elen &&
                !memcmp(seen->s + seen->len - elen, e, elen)) {
                if (autologon_advance(ws, true))
                    send = true;
            } else if (seen->len > 4096 && seen->len > elen) {
                /* Keep only what could still be the start of it */
                size_t keep = elen - 1;
                memmove(seen->s, seen->s + seen->len - keep, keep);
                strbuf_shrink_to(seen, keep);
            }
        }
    }

    if (send)
        queue_toplevel_callback(autologon_send_callback,
                                (void *)(uintptr_t)ws->id);
    size_t ret = term_data(ws->wgs.term, out->s, out->len);
    strbuf_free(out);
    return ret;
}

/* One of PuTTY's own prompts: answer it, if the next steps expect
 * exactly these prompts */
static bool autologon_prompts(WuiSeat *ws, prompts_t *p)
{
    if (!p->n_prompts || ws->al_sent < ws->al_step ||
        ws->al_step + p->n_prompts > ws->al_nsteps)
        return false;
    for (size_t i = 0; i < p->n_prompts; i++) {
        const char *e = ws->al_steps[ws->al_step + i].expect;
        if (!*e || !strstr(p->prompts[i]->prompt, e)) {
            logeventf(ws->wgs.logctx, "Automate logon: prompt \"%s\" "
                      "isn't the \"%s\" that step %u is waiting for",
                      p->prompts[i]->prompt, e,
                      (unsigned)(ws->al_step + i) + 1);
            return false;
        }
    }
    for (size_t i = 0; i < p->n_prompts; i++) {
        prompt_t *pr = p->prompts[i];
        AutologonStep *st = &ws->al_steps[ws->al_step++];
        prompt_set_result(pr, st->command);
        /* Show it as if it had been typed in */
        term_data(ws->wgs.term, pr->prompt, strlen(pr->prompt));
        if (pr->echo && !st->hide)
            term_data(ws->wgs.term, st->command, strlen(st->command));
        term_data(ws->wgs.term, "\r\n", 2);
        logeventf(ws->wgs.logctx, "Automate logon: answered prompt %u%s",
                  (unsigned)ws->al_step, st->hide ? " (hidden)" : "");
    }
    ws->al_sent = ws->al_step;
    return true;
}

static bool is_utf8(WinGuiSeat *wgs)
{
    return wgs->ucsdata.line_codepage == CP_UTF8;
}

static bool win_seat_is_utf8(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return is_utf8(wgs);
}

static char *win_seat_get_ttymode(Seat *seat, const char *mode)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return term_get_ttymode(wgs->term, mode);
}

static StripCtrlChars *win_seat_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    return stripctrl_new_term(bs_out, false, 0, wgs->term);
}

static size_t win_seat_output(
    Seat *seat, SeatOutputType type, const void *, size_t);
static bool win_seat_eof(Seat *seat);
static SeatPromptResult win_seat_get_userpass_input(Seat *seat, prompts_t *p);
static void win_seat_notify_remote_exit(Seat *seat);
static void win_seat_connection_fatal(Seat *seat, const char *msg);
static void win_seat_nonfatal(Seat *seat, const char *msg);
static void win_seat_update_specials_menu(Seat *seat);
static void win_seat_set_busy_status(Seat *seat, BusyStatus status);
static void win_seat_set_trust_status(Seat *seat, bool trusted);
static bool win_seat_can_set_trust_status(Seat *seat);
static bool win_seat_get_cursor_position(Seat *seat, int *x, int *y);
static bool win_seat_get_window_pixel_size(Seat *seat, int *x, int *y);
static SeatPromptResult wui_seat_confirm_ssh_host_key(
    Seat *seat, const char *host_name, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx);
static SeatPromptResult wui_seat_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx);
static SeatPromptResult wui_seat_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx);

static const SeatVtable win_seat_vt = {
    .output = win_seat_output,
    .eof = win_seat_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = win_seat_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = win_seat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = win_seat_connection_fatal,
    .nonfatal = win_seat_nonfatal,
    .update_specials_menu = win_seat_update_specials_menu,
    .get_ttymode = win_seat_get_ttymode,
    .set_busy_status = win_seat_set_busy_status,
    .confirm_ssh_host_key = wui_seat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = wui_seat_confirm_weak_crypto_primitive,
    .confirm_weak_cached_hostkey = wui_seat_confirm_weak_cached_hostkey,
    .prompt_descriptions = win_seat_prompt_descriptions,
    .is_utf8 = win_seat_is_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = win_seat_get_window_pixel_size,
    .stripctrl_new = win_seat_stripctrl_new,
    .set_trust_status = win_seat_set_trust_status,
    .can_set_trust_status = win_seat_can_set_trust_status,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_yes,
    .verbose = nullseat_verbose_yes,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = win_seat_get_cursor_position,
};

/*
 * Starting a session. The backend looks up the host name as it
 * starts, which can take seconds (for a name that doesn't exist, as
 * Windows tries one way after another), and that would freeze the
 * whole window, all its tabs. So first it's looked up in a thread of
 * its own, the tab saying so meanwhile; then the backend's own lookup
 * is quick, as Windows has the answer by then. (Not for a serial
 * line, or through a proxy, which may look names up itself.)
 */
typedef struct HostLookup {
    unsigned seat_id, serial;
    char *host;
    int family;
    char *error;                       /* or NULL, if it was found */
} HostLookup;

static void connect_backend(WinGuiSeat *wgs);

static DWORD WINAPI lookup_thread(void *vctx)
{
    HostLookup *hl = (HostLookup *)vctx;
    char *canonical = NULL;
    SockAddr *addr = sk_namelookup(hl->host, &canonical, hl->family);
    const char *err = sk_addr_error(addr);
    hl->error = err ? dupstr(err) : NULL;
    sk_addr_free(addr);
    sfree(canonical);
    PostMessage(msg_hwnd, WM_WUI_LOOKUP, 0, (LPARAM)hl);
    return 0;
}

static void lookup_free(HostLookup *hl)
{
    sfree(hl->host);
    sfree(hl->error);
    sfree(hl);
}

/* (as a toplevel callback, so not in the middle of a dialog box) */
static void lookup_done(void *vctx)
{
    HostLookup *hl = (HostLookup *)vctx;
    WuiSeat *ws = find_seat(hl->seat_id);
    /* (unless it's gone, or started again, meanwhile) */
    if (ws && ws->lookup == hl->serial) {
        WinGuiSeat *wgs = &ws->wgs;
        ws->lookup = 0;
        term_data(wgs->term, "\r\033[K", 4);   /* (no more "Looking up") */
        if (hl->error) {
            char *msg = dupprintf("Unable to open connection to %s: %s",
                                  conf_dest(wgs->conf), hl->error);
            win_seat_connection_fatal(&wgs->seat, msg);
            sfree(msg);
        } else {
            connect_backend(wgs);
        }
    }
    lookup_free(hl);
}

static void start_backend(WinGuiSeat *wgs)
{
    WuiSeat *ws = WUI(wgs);
    static unsigned lookup_serial;

    /* (restarting the welcome tab makes it an ordinary one) */
    if (ws->welcome && ws->tterm)
        wui_term_set_hint(ws->tterm, NULL);
    ws->welcome = false;
    if (ws->tterm)
        wui_term_set_status(ws->tterm, NULL);   /* (a restart) */

    const char *host = conf_get_str(wgs->conf, CONF_host);
    int proto = conf_get_int(wgs->conf, CONF_protocol);
    if (!*host || proto == PROT_SERIAL ||
        conf_get_int(wgs->conf, CONF_proxy_type) != PROXY_NONE) {
        connect_backend(wgs);
        return;
    }

    if (!++lookup_serial)
        lookup_serial++;                /* (never 0, which is none) */
    HostLookup *hl = snew(HostLookup);
    hl->seat_id = ws->id;
    hl->serial = ws->lookup = lookup_serial;
    /* (not any user name, in case prepare_session hasn't been at it) */
    const char *at = strrchr(host, '@');
    hl->host = dupstr(at ? at + 1 : host);
    hl->family = conf_get_int(wgs->conf, CONF_addressfamily);
    hl->error = NULL;
    HANDLE thread = CreateThread(NULL, 0, lookup_thread, hl, 0, NULL);
    if (!thread) {
        ws->lookup = 0;
        lookup_free(hl);
        connect_backend(wgs);
        return;
    }
    CloseHandle(thread);
    char *text = dupprintf("Looking up %s...", host);
    term_data(wgs->term, text, strlen(text));
    sfree(text);
}

static void connect_backend(WinGuiSeat *wgs)
{
    const struct BackendVtable *vt;
    char *error, *realhost;

    wgs->cmdline_get_passwd_state = cmdline_get_passwd_input_state_new;
    autologon_start(WUI(wgs));

    vt = backend_vt_from_conf(wgs->conf);

    seat_set_trust_status(&wgs->seat, true);
    error = backend_init(vt, &wgs->seat, &wgs->backend, wgs->logctx, wgs->conf,
                         conf_get_str(wgs->conf, CONF_host),
                         conf_get_int(wgs->conf, CONF_port),
                         &realhost,
                         conf_get_bool(wgs->conf, CONF_tcp_nodelay),
                         conf_get_bool(wgs->conf, CONF_tcp_keepalives));
    if (error) {
        /*
         * Not exiting, as window.c does: there may be other tabs. It's
         * as if the connection had been made and then lost, with the
         * message in the terminal and the tab's warning icon, and the
         * session closed (or its tab, if it's set to close on exit),
         * so that it can be restarted.
         */
        char *msg;
        if (cmdline_tooltype & TOOLTYPE_NONNETWORK) {
            /* Special case for pterm. */
            msg = dupprintf("Unable to open terminal: %s", error);
        } else {
            msg = dupprintf("Unable to open connection to %s: %s",
                            conf_dest(wgs->conf), error);
        }
        sfree(error);
        win_seat_connection_fatal(&wgs->seat, msg);
        sfree(msg);
        return;
    }
    term_setup_window_titles(wgs->term, realhost);
    sfree(realhost);

    /*
     * Connect the terminal to the backend for resize purposes.
     */
    term_provide_backend(wgs->term, wgs->backend);

    /*
     * Set up a line discipline.
     */
    wgs->ldisc = ldisc_create(wgs->conf, wgs->term, wgs->backend, &wgs->seat);

    /* The Restart Session menu item is shown only while session_closed
     * is set, so clearing it is all we need to do to hide it. */
    wgs->session_closed = false;
}

static void close_session(void *vctx)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    char *newtitle;

    wgs->session_closed = true;
    newtitle = dupprintf("%s (inactive)", appname);
    win_set_icon_title(&wgs->termwin, newtitle, DEFAULT_CODEPAGE);
    win_set_title(&wgs->termwin, newtitle, DEFAULT_CODEPAGE);
    sfree(newtitle);

    if (wgs->ldisc) {
        ldisc_free(wgs->ldisc);
        wgs->ldisc = NULL;
    }
    xfer_stop(WUI(wgs));
    if (wgs->backend) {
        backend_free(wgs->backend);
        wgs->backend = NULL;
        term_provide_backend(wgs->term, NULL);
        seat_update_specials_menu(&wgs->seat);
    }
}

HINSTANCE hinst;

/* ----------------------------------------------------------------------
 * Event loop integration.
 *
 * The XAML framework owns the message loop, so instead of sitting in
 * MsgWaitForMultipleObjects as window.c does, we arrange for
 * everything PuTTY needs to wait for to turn into a window message
 * sent to a hidden window of our own:
 *
 *  - network events already arrive as WM_NETEVENT (select-gui.c);
 *  - timers already arrive as WM_TIMER (gui-timing.c);
 *  - toplevel callbacks ask us to post WM_WUI_CALLBACKS when the
 *    queue becomes non-empty;
 *  - each HANDLE that handle-wait.c wants watched gets a one-shot
 *    thread-pool wait, which posts WM_WUI_HANDLEWAIT when it fires.
 *
 * The old main loop never ran callbacks or handle waits from inside a
 * modal dialog box opened by one of them (e.g. the host key prompt).
 * We preserve that by deferring both while the front end is 'busy',
 * i.e. while we're inside something that might run a nested message
 * loop.
 */

static bool callbacks_enabled;         /* not until the window is up */
static bool callback_msg_pending;
static int frontend_busy;

static void post_callback_msg(void)
{
    if (!callback_msg_pending && msg_hwnd) {
        callback_msg_pending = true;
        PostMessage(msg_hwnd, WM_WUI_CALLBACKS, 0, 0);
    }
}

static void callback_notify(void *ctx)
{
    post_callback_msg();
}

static void run_callbacks(void)
{
    if (!callbacks_enabled || frontend_busy)
        return;           /* frontend_busy_leave() will post again */

    frontend_busy_enter();
    /*
     * Run callbacks for a bounded time, so that a stream of callbacks
     * that keep rescheduling themselves can't starve user input.
     */
    DWORD start = GetTickCount();
    while (run_toplevel_callbacks()) {
        if (GetTickCount() - start >= 20)
            break;
    }
    frontend_busy_leave();
}

typedef struct WuiWait WuiWait;
struct WuiWait {
    HandleWait *hw;
    HANDLE wait;
    unsigned serial;
    bool deferred;
    WuiWait *next;
};
static WuiWait *waits;
static unsigned wait_serial;

static VOID CALLBACK wait_fired(PVOID param, BOOLEAN timed_out)
{
    WuiWait *w = (WuiWait *)param;
    PostMessage(msg_hwnd, WM_WUI_HANDLEWAIT, (WPARAM)w, (LPARAM)w->serial);
}

static void arm_wait(WuiWait *w)
{
    if (!RegisterWaitForSingleObject(
            &w->wait, handle_wait_get_handle(w->hw), wait_fired, w,
            INFINITE, WT_EXECUTEONLYONCE))
        w->wait = NULL;
}

static WuiWait *find_wait(WuiWait *w, unsigned serial)
{
    for (WuiWait *v = waits; v; v = v->next)
        if (v == w && v->serial == serial)
            return v;
    return NULL;
}

static void handle_wait_hook(void *ctx, HandleWait *hw, bool added)
{
    if (added) {
        WuiWait *w = snew(WuiWait);
        w->hw = hw;
        w->serial = ++wait_serial;
        w->deferred = false;
        w->next = waits;
        waits = w;
        arm_wait(w);
    } else {
        for (WuiWait **pw = &waits; *pw; pw = &(*pw)->next) {
            WuiWait *w = *pw;
            if (w->hw == hw) {
                *pw = w->next;
                /* Wait for any in-progress callback to finish, so that
                 * nothing still refers to w once we free it. */
                if (w->wait)
                    UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE);
                sfree(w);
                break;
            }
        }
    }
}

static void handle_wait_message(WuiWait *w, unsigned serial)
{
    if (!(w = find_wait(w, serial)))
        return;                        /* deleted since it fired */

    if (frontend_busy) {
        w->deferred = true;
        return;
    }

    if (w->wait) {
        UnregisterWaitEx(w->wait, NULL);
        w->wait = NULL;
    }

    frontend_busy_enter();
    handle_wait_trigger(w->hw);
    frontend_busy_leave();

    /* The callback might have deleted the wait. If not, re-arm it, so
     * that it fires again if the handle is still signalled, just as
     * it would be in a MsgWaitForMultipleObjects loop. */
    if ((w = find_wait(w, serial)) != NULL && !w->wait)
        arm_wait(w);
}

static void frontend_busy_enter(void)
{
    frontend_busy++;
}

static void frontend_busy_leave(void)
{
    assert(frontend_busy > 0);
    if (--frontend_busy)
        return;

    /* Now catch up on anything we put off. */
    if (toplevel_callback_pending())
        post_callback_msg();
    for (WuiWait *w = waits; w; w = w->next) {
        if (w->deferred) {
            w->deferred = false;
            PostMessage(msg_hwnd, WM_WUI_HANDLEWAIT,
                        (WPARAM)w, (LPARAM)w->serial);
        }
    }
}

static void confirm_close_window(void);
static void confirm_close_tab(WinGuiSeat *wgs);
static void confirm_close_session(WinGuiSeat *wgs);

struct clipdata_msg {
    unsigned seat_id;
    bool unicode;
};

static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam)
{
    switch (message) {
      case WM_NETEVENT:
      case WM_DONE_WITH_SOCKET:
        winselgui_response(message, wParam, lParam);
        return 0;
      case WM_WUI_LOOKUP:
        queue_toplevel_callback(lookup_done, (void *)lParam);
        return 0;
      case WM_WUI_CALLBACKS:
        callback_msg_pending = false;
        run_callbacks();
        if (!frontend_busy && callbacks_enabled &&
            toplevel_callback_pending())
            post_callback_msg();
        return 0;
      case WM_WUI_HANDLEWAIT:
        handle_wait_message((WuiWait *)wParam, (unsigned)lParam);
        return 0;
      case WM_WUI_COMMAND: {
        /* lParam says which session the command came from; if that's
         * gone away, or it's a window-level command, use the
         * current one */
        WuiSeat *ws = find_seat((unsigned)lParam);
        if (!ws)
            ws = active;
        if (ws)
            do_command(&ws->wgs, (unsigned)wParam);
        return 0;
      }
      case WM_WUI_CLOSE:
        confirm_close_window();
        return 0;
      case WM_WUI_CLOSE_TAB: {
        WuiSeat *ws = find_seat((unsigned)wParam);
        if (ws)
            confirm_close_tab(&ws->wgs);
        return 0;
      }
      case WM_WUI_CLOSE_PANE: {
        WuiSeat *ws = find_seat((unsigned)wParam);
        if (ws)
            confirm_close_session(&ws->wgs);
        return 0;
      }
      case WM_IGNORE_CLIP:
        ignore_clip = wParam;          /* don't panic on DESTROYCLIPBOARD */
        return 0;
      case WM_DESTROYCLIPBOARD: {
        WuiSeat *ws = find_seat(clip_owner);
        if (ws && !ignore_clip)
            term_lost_clipboard_ownership(ws->wgs.term, CLIP_SYSTEM);
        ignore_clip = false;
        return 0;
      }
      case WM_GOT_CLIPDATA: {
        const struct clipdata_msg *cm = (const struct clipdata_msg *)wParam;
        WuiSeat *ws = find_seat(cm->seat_id);
        if (ws)
            process_clipdata(&ws->wgs, (HGLOBAL)lParam, cm->unicode);
        return 0;
      }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

static void create_msg_window(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"PuTTYWinUIMessageWindow";
    RegisterClassW(&wc);

    /* A real (hidden) top-level window rather than a message-only one,
     * because it also serves as our clipboard owner. */
    msg_hwnd = CreateWindowW(
        wc.lpszClassName, WUI_APP_NAME L": hidden message window",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        100, 100, NULL, NULL, hinst, NULL);
    if (!msg_hwnd)
        modalfatalbox("Unable to create message window: %s",
                      win_strerror(GetLastError()));
}

/* ----------------------------------------------------------------------
 * Keyboard input.
 *
 * PuTTY does its own translation of WM_KEYDOWN into terminal input
 * (TranslateKey below), and must not let TranslateMessage see the
 * keystrokes first, or dead keys and Alt-prefixing go wrong. The XAML
 * message loop calls TranslateMessage on everything it retrieves, so
 * we intercept key messages for the terminal with a WH_GETMESSAGE
 * hook, which sees each message as it leaves the queue, before the
 * loop does anything with it. Messages we fully handle are turned
 * into WM_NULL; the rest (IME, VK_PACKET, and keys like Alt+F4 that
 * TranslateKey wants Windows to handle) continue on to XAML, and any
 * characters they generate come back to us via wui_cb_char.
 *
 * Keystrokes always go to the session in the visible tab.
 */

static bool is_terminal_hwnd(WinGuiSeat *wgs, HWND hwnd)
{
    return wgs->term_hwnd &&
        (hwnd == wgs->term_hwnd || IsChild(wgs->term_hwnd, hwnd));
}

static bool handle_key_message(WinGuiSeat *wgs, MSG *m)
{
    unsigned char buf[20];
    int len;

    /*
     * Add the scan code and keypress timing to the random
     * number noise.
     */
    noise_ultralight(NOISE_SOURCE_KEY, m->lParam);

    if (m->wParam == VK_PROCESSKEY || /* IME PROCESS key */
        m->wParam == VK_PACKET)       /* 'this key is a Unicode char' */
        return false;                 /* let the IME / XAML deal with it */

    /* Ctrl+Tab and Ctrl+Shift+Tab switch tabs. (In a lone tab,
     * they're left alone; PuTTY sends plain Tab for them anyway.) */
    if (m->wParam == VK_TAB && nseats > 1 &&
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000)) {
        if (m->message == WM_KEYDOWN)
            wui_select_adjacent_tab(
                host, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1);
        return true;
    }

    /* (JoliPuTTY's own shortcuts, below, unless the session says to
     * leave those keys to the server: Terminal, Keyboard shortcuts) */
    bool own = !conf_get_bool(wgs->conf, CONF_no_wui_shortcuts);

    /* Ctrl+comma: the session's settings (as in Windows Terminal; Ctrl
     * with a comma is no control character, so it sends nothing that
     * the server could have wanted) */
    if (own && m->wParam == VK_OEM_COMMA && !WUI(wgs)->welcome &&
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (m->message == WM_KEYDOWN)
            PostMessage(msg_hwnd, WM_WUI_COMMAND, IDM_RECONF, WUI(wgs)->id);
        return true;
    }

    /* Ctrl+(left) Alt+V: the pools of text to paste from (not with
     * AltGr, which is Ctrl and the right Alt, for what that types) */
    if (own && m->wParam == 'V' && (GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_LMENU) & 0x8000) &&
        !(GetKeyState(VK_RMENU) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)
            PostMessage(msg_hwnd, WM_WUI_COMMAND, IDM_POOL_PASTE,
                        WUI(wgs)->id);
        return true;
    }

    /* Ctrl+(left) Alt+1 to 9: the n-th tab, or the last (as in Windows
     * Terminal; not with AltGr, as for Ctrl+Alt+V) */
    if (own && m->wParam >= '1' && m->wParam <= '9' && nseats > 1 &&
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_LMENU) & 0x8000) &&
        !(GetKeyState(VK_RMENU) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)
            wui_select_tab_number(host, (int)(m->wParam - '0'));
        return true;
    }

    /* (Left) Alt+arrow: the pane next to this one that way, in a split
     * tab (as in Windows Terminal). In a tab that isn't split, or with
     * no pane that way, the key goes to the session as ever. */
    if (own && (m->wParam == VK_LEFT || m->wParam == VK_RIGHT ||
         m->wParam == VK_UP || m->wParam == VK_DOWN) &&
        WUI(wgs)->tterm && wui_term_is_split(WUI(wgs)->tterm) &&
        (GetKeyState(VK_LMENU) & 0x8000) &&
        !(GetKeyState(VK_RMENU) & 0x8000) &&
        !(GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000)) {
        int dx = m->wParam == VK_LEFT ? -1 : m->wParam == VK_RIGHT ? +1 : 0;
        int dy = m->wParam == VK_UP ? -1 : m->wParam == VK_DOWN ? +1 : 0;
        if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN) &&
            wui_term_focus_pane(WUI(wgs)->tterm, dx, dy))
            return true;
        /* (its key up goes to the session, which doesn't mind one it
         * didn't see go down) */
    }

    /* (Left) Alt+Shift+plus and minus: split the pane, to the right or
     * down; and Ctrl+Shift+W, in a split tab, closes it (as in Windows
     * Terminal). Plus and minus on the keypad, which are where they are
     * whatever the keyboard, as well as the main ones, where they're on
     * keys of their own. */
    if (own && (m->wParam == VK_ADD || m->wParam == VK_OEM_PLUS ||
         m->wParam == VK_SUBTRACT || m->wParam == VK_OEM_MINUS) &&
        !WUI(wgs)->welcome &&
        (GetKeyState(VK_LMENU) & 0x8000) &&
        !(GetKeyState(VK_RMENU) & 0x8000) &&
        !(GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_SHIFT) & 0x8000)) {
        if (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)
            PostMessage(msg_hwnd, WM_WUI_COMMAND,
                        m->wParam == VK_ADD || m->wParam == VK_OEM_PLUS ?
                        IDM_SPLITRIGHT : IDM_SPLITDOWN, WUI(wgs)->id);
        return true;
    }
    if (own && m->wParam == 'W' && WUI(wgs)->tterm &&
        wui_term_is_split(WUI(wgs)->tterm) &&
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_SHIFT) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000)) {
        if (m->message == WM_KEYDOWN)
            PostMessage(msg_hwnd, WM_WUI_CLOSE_PANE, WUI(wgs)->id, 0);
        return true;
    }

    /* Ctrl+Shift+Space: the saved sessions' pop-up (as in Windows
     * Terminal, for its own) */
    if (own && m->wParam == VK_SPACE &&
        (GetKeyState(VK_CONTROL) & 0x8000) &&
        (GetKeyState(VK_SHIFT) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000)) {
        if (m->message == WM_KEYDOWN)
            wui_show_sessions_popup(host);
        return true;
    }

    /* Ctrl+F4 closes the tab, as its close button does */
    if (m->wParam == VK_F4 && (GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000) &&
        !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (m->message == WM_KEYDOWN)
            PostMessage(msg_hwnd, WM_WUI_CLOSE_TAB, WUI(wgs)->id, 0);
        return true;
    }

    /* Track the input locale (WM_INPUTLANGCHANGE goes to XAML's
     * input window, not to us). */
    set_input_locale(wgs, GetKeyboardLayout(0));

    len = TranslateKey(wgs, m->message, m->wParam, m->lParam, buf);
    if (len == -1)
        return false;                 /* default Windows handling */

    if (len != 0) {
        /*
         * We need not bother about stdin backlogs here, because in
         * GUI PuTTY we can't do anything about it anyway; there's no
         * means of asking Windows to hold off on KEYDOWN messages. We
         * _have_ to buffer everything we're sent.
         */
        term_keyinput(wgs->term, -1, buf, len);
        show_mouseptr(wgs, false);
    }
    return true;
}

static LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        MSG *m = (MSG *)lParam;
        HWND root = m->hwnd ? GetAncestor(m->hwnd, GA_ROOT) : NULL;
        char cls[16];

        /* Event Logs are modeless dialogs (one per session), so they
         * need IsDialogMessage to get keyboard navigation. */
        if (root && root != m->hwnd && host && root != wui_hwnd(host) &&
            GetClassName(root, cls, sizeof(cls)) && !strcmp(cls, "#32770") &&
            GetWindow(root, GW_OWNER) == wui_hwnd(host) &&
            IsDialogMessage(root, m)) {
            m->message = WM_NULL;
        } else if ((m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN ||
                    m->message == WM_KEYUP || m->message == WM_SYSKEYUP) &&
                   active && active->terminal_focused && active->wgs.term &&
                   is_terminal_hwnd(&active->wgs, m->hwnd)) {
            if (handle_key_message(&active->wgs, m))
                m->message = WM_NULL;
        }
    }
    return CallNextHookEx(key_hook, code, wParam, lParam);
}

/*
 * Find: every match of the text in the terminal, scrollback and all
 * (within a line: one wrapped onto the next isn't found), in order.
 */
static TermHighlight *find_all(Terminal *term, const wchar_t *text,
                           bool match_case, WuiRegex *re, size_t *nfound)
{
    TermHighlight *found = NULL;
    size_t n = 0, size = 0;
    size_t tlen = wcslen(text);
    wchar_t *pattern = dupwcs(text);
    if (!match_case)
        CharLowerBuffW(pattern, (DWORD)tlen);

    for (int y = term_first_line(term); y < term->rows; y++) {
        int *cols;
        size_t len;
        wchar_t *line = term_line_text(term, y, &cols, &len);
        if (!match_case && !re)
            CharLowerBuffW(line, (DWORD)len);

        size_t from = 0, start, mlen;
        while (from < len) {
            if (re) {
                if (!wui_regex_find(re, line, len, from, &start, &mlen))
                    break;
                if (mlen == 0) {       /* (matching nothing isn't found) */
                    from = start + 1;
                    continue;
                }
            } else {
                wchar_t *p = wcsstr(line + from, pattern);
                if (!p)
                    break;
                start = p - line;
                mlen = tlen;
            }
            size_t end = start + mlen;
            sgrowarray(found, size, n);
            found[n].y = y;
            found[n].x0 = cols[start];
            found[n].x1 = end < len ? cols[end] : cols[end - 1] + 1;
            n++;
            from = end;
        }
        sfree(line);
        sfree(cols);
    }
    sfree(pattern);
    *nfound = n;
    return found;
}

static bool found_before(const TermHighlight *f, int y, int x)
{
    return f->y < y || (f->y == y && f->x0 < x);
}

void wui_cb_search(void *ctx, const wchar_t *text, bool match_case,
                   bool regex, int dir)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    Terminal *term = ws->wgs.term;
    if (!term)
        return;
    if (!*text) {
        ws->found = false;
        term_set_highlights(term, NULL, 0, 0);
        wui_term_search_result(ws->tterm, 0, 0);
        return;
    }

    WuiRegex *re = NULL;
    if (regex && !(re = wui_regex_new(text, !match_case))) {
        term_set_highlights(term, NULL, 0, 0);
        wui_term_search_result(ws->tterm, 0, -1);
        return;
    }
    size_t n;
    TermHighlight *found = find_all(term, text, match_case, re, &n);
    if (re)
        wui_regex_free(re);
    if (n == 0) {
        ws->found = false;
        term_set_highlights(term, NULL, 0, 0);
        wui_term_search_result(ws->tterm, 0, 0);
        sfree(found);
        return;
    }

    /*
     * Which one: the first time, the last (nearest the bottom, where
     * the latest output is, and searching goes upwards from there);
     * after that, the one before or after the last one found, going
     * round at the ends; or, as the text is typed, the first at or
     * after it, so that it stays put while it still matches.
     */
    size_t i;
    if (!ws->found) {
        i = (dir > 0 ? 0 : n - 1);
    } else if (dir < 0) {
        for (i = n; i > 0; i--)
            if (found_before(&found[i - 1], ws->found_y, ws->found_x))
                break;
        i = (i > 0 ? i - 1 : n - 1);
    } else {
        int x = ws->found_x + (dir > 0 ? 1 : 0);
        for (i = 0; i < n; i++)
            if (!found_before(&found[i], ws->found_y, x))
                break;
        if (i == n)
            i = 0;
    }

    ws->found = true;
    ws->found_y = found[i].y;
    ws->found_x = found[i].x0;
    term_select_range(term, found[i].y, found[i].x0,
                      found[i].y, found[i].x1);
    term_set_highlights(term, found, n, i);
    wui_term_search_result(ws->tterm, (int)i + 1, (int)n);
    sfree(found);
}

void wui_cb_search_closed(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    ws->found = false;
    if (ws->wgs.term)
        term_set_highlights(ws->wgs.term, NULL, 0, 0);
}

/* ----------------------------------------------------------------------
 * Send File and Receive File: transfer a file to or from the server
 * by SFTP (over channels of their own in the SSH session). Files are
 * sent to, or looked for in, the directory the shell says it's in if
 * it's told us (by OSC 7), or else where the user says, with the help
 * of a listing of the remote directory as they type.
 */

typedef enum XferKind {
    XK_HOME,                           /* finding the home directory */
    XK_SEND,                           /* sending a file */
    XK_CHECK,                          /* checking a file to receive */
    XK_RECEIVE                         /* receiving it */
} XferKind;

/* Where files have been sent lately, most recent first, for all the
 * sessions (since they're often to the same places) */
#define SEND_RECENT_MAX 8
static wchar_t *send_recent[SEND_RECENT_MAX];
static size_t n_send_recent;

static void send_recent_add(const wchar_t *dir)
{
    size_t i;
    for (i = 0; i < n_send_recent; i++)
        if (!wcscmp(send_recent[i], dir))
            break;
    wchar_t *entry;
    if (i < n_send_recent) {
        entry = send_recent[i];        /* (just move it to the top) */
    } else {
        entry = dupwcs(dir);
        if (n_send_recent == SEND_RECENT_MAX)
            sfree(send_recent[--n_send_recent]);
        i = n_send_recent++;
    }
    memmove(send_recent + 1, send_recent, i * sizeof(*send_recent));
    send_recent[0] = entry;
}

static wchar_t *utf8_to_wide(const char *s)
{
    return dup_mb_to_wc(CP_UTF8, s);
}

static char *wide_to_utf8(const wchar_t *s)
{
    return dup_wc_to_mb(CP_UTF8, s, "?");
}

/* A size, as people like to read them */
static wchar_t *size_text(wchar_t *buf, size_t len, uint64_t size)
{
    static const wchar_t *const units[] = { L"KB", L"MB", L"GB", L"TB" };
    if (size < 1024) {
        swprintf(buf, len, L"%u bytes", (unsigned)size);
        return buf;
    }
    double v = (double)size / 1024;
    int u = 0;
    while (v >= 1024 && u < (int)lenof(units) - 1) {
        v /= 1024;
        u++;
    }
    swprintf(buf, len, v < 10 ? L"%.1f %ls" : L"%.0f %ls", v, units[u]);
    return buf;
}

/* The last part of a path, after any of the separators in 'seps' */
static const char *path_leaf(const char *path, const char *seps)
{
    const char *leaf = path;
    for (const char *p = path; *p; p++)
        if (strchr(seps, *p))
            leaf = p + 1;
    return leaf;
}

/* Is the bar busy with a file (so that it can't start another)? */
static bool xfer_busy(WuiSeat *ws)
{
    return (ws->xfer && ws->xfer_kind != XK_HOME) || ws->recv_asking;
}

static void list_stop(WuiSeat *ws)
{
    if (ws->list_xfer) {
        sftp_xfer_cancel(ws->list_xfer);
        ws->list_xfer = NULL;
    }
    for (size_t i = 0; i < ws->n_list; i++)
        sfree(ws->list_names[i]);
    ws->n_list = 0;
    sfree(ws->list_dir);
    ws->list_dir = NULL;
}

/* Stop whatever the bar's doing (and forget a half-received file) */
static void xfer_stop(WuiSeat *ws)
{
    if (ws->xfer) {
        sftp_xfer_cancel(ws->xfer);    /* (which closes the file) */
        ws->xfer = NULL;
        if (ws->xfer_kind == XK_RECEIVE && ws->recv_part)
            DeleteFileW(ws->recv_part);
    }
    ws->recv_asking = false;
    list_stop(ws);
}

/* All of it, when the session's going */
static void xfer_free(WuiSeat *ws)
{
    xfer_stop(ws);
    if (ws->send_file)
        filename_free(ws->send_file);
    ws->send_file = NULL;
    sfree(ws->send_dir);
    sfree(ws->recv_path);
    sfree(ws->recv_dir);
    sfree(ws->recv_local);
    sfree(ws->recv_part);
    ws->send_dir = ws->recv_path = ws->recv_dir = NULL;
    ws->recv_local = ws->recv_part = NULL;
}

/* The system's file dialog, to open or save a file */
static wchar_t *file_dialog(bool save, const wchar_t *title,
                            const GUID *which, const wchar_t *name)
{
    /* (here rather than from uuid.lib) */
    static const CLSID clsid_FileOpenDialog = {
        0xDC1C5A9C, 0xE88A, 0x4DDE,
        { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
    static const CLSID clsid_FileSaveDialog = {
        0xC0B4E2F3, 0xBA21, 0x4773,
        { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };
    static const IID iid_IFileDialog = {
        0x42F85136, 0xDB7E, 0x439C,
        { 0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8 } };

    IFileDialog *dlg;
    wchar_t *result = NULL;
    if (FAILED(CoCreateInstance(save ? &clsid_FileSaveDialog :
                                &clsid_FileOpenDialog, NULL,
                                CLSCTX_INPROC_SERVER, &iid_IFileDialog,
                                (void **)&dlg)))
        return NULL;

    FILEOPENDIALOGOPTIONS opts;
    if (SUCCEEDED(dlg->lpVtbl->GetOptions(dlg, &opts)))
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_FORCEFILESYSTEM |
                                FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                                (save ? FOS_OVERWRITEPROMPT :
                                 FOS_FILEMUSTEXIST));
    dlg->lpVtbl->SetTitle(dlg, title);
    /* (so that it remembers its own folder, apart from other dialogs) */
    dlg->lpVtbl->SetClientGuid(dlg, which);
    if (name)
        dlg->lpVtbl->SetFileName(dlg, name);

    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, wui_hwnd(host)))) {
        IShellItem *item;
        if (SUCCEEDED(dlg->lpVtbl->GetResult(dlg, &item))) {
            wchar_t *path;
            if (SUCCEEDED(item->lpVtbl->GetDisplayName(
                              item, SIGDN_FILESYSPATH, &path))) {
                result = dupwcs(path);
                CoTaskMemFree(path);
            }
            item->lpVtbl->Release(item);
        }
    }
    dlg->lpVtbl->Release(dlg);
    return result;
}

static const GUID send_file_dialog = {
    0x6F3A2C51, 0x9B7E, 0x4C1D,
    { 0x8E, 0x24, 0x5A, 0x91, 0xC7, 0x03, 0xB6, 0x4F } };
static const GUID receive_file_dialog = {
    0x2D8E4B17, 0x63C9, 0x4A05,
    { 0xB1, 0x7F, 0x0C, 0x52, 0xE9, 0x38, 0x4A, 0xD6 } };

/* ---- Finding the home directory ---- */

static void home_done(void *ctx, SftpXferResult result, const char *text)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    ws->xfer = NULL;
    if (result == SFTP_XFER_OK) {
        /* (when receiving, it's the start of the file's path) */
        wchar_t *home = utf8_to_wide(text);
        wchar_t *dest = ws->bar_receive && *home &&
            home[wcslen(home) - 1] != L'/' ?
            dupwcscat(home, L"/") : dupwcs(home);
        wui_term_send_dest(ws->tterm, dest);
        sfree(dest);
        sfree(home);
    }
    /* (and if not, we'll find out again, properly, later) */
}

static const SftpXferClient home_client = {
    .done = home_done,
};

/* Show the bar, starting from the directory the shell's in, or the
 * last one used, or the home directory once we know where that is */
static void xfer_bar_start(WuiSeat *ws, bool receive, const wchar_t *name)
{
    WinGuiSeat *wgs = &ws->wgs;
    xfer_stop(ws);
    ws->bar_receive = receive;

    const char *cwd = wgs->term ? term_remote_cwd(wgs->term) : NULL;
    const char *dir = cwd ? cwd : receive && ws->recv_dir ? ws->recv_dir :
        ws->send_dir;
    wchar_t *wdir = utf8_to_wide(dir ? dir : "");
    if (!receive) {
        wui_term_show_send(ws->tterm, name, wdir,
                           (const wchar_t *const *)send_recent,
                           n_send_recent, cwd != NULL);
    } else {
        wchar_t *path = *wdir && wdir[wcslen(wdir) - 1] != L'/' ?
            dupwcscat(wdir, L"/") : dupwcs(wdir);
        wui_term_show_receive(ws->tterm, path, cwd != NULL);
        sfree(path);
    }
    sfree(wdir);

    if (!dir) {
        ws->xfer_kind = XK_HOME;
        ws->xfer = sftp_xfer_home(wgs->backend, &home_client, ws);
    }
}

void wui_cb_send_closed(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    xfer_stop(ws);
    if (ws->send_file) {
        filename_free(ws->send_file);
        ws->send_file = NULL;
    }
}

/* ---- Listing a remote directory, for completing what's typed ---- */

static void list_entry(void *ctx, const char *name, bool is_dir)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    wchar_t *wname = utf8_to_wide(name);
    sgrowarray(ws->list_names, ws->list_size, ws->n_list);
    ws->list_names[ws->n_list++] =
        is_dir ? dupwcscat(wname, L"/") : dupwcs(wname);
    sfree(wname);
}

/* Directories first, then files, each alphabetically */
static int list_cmp(const void *av, const void *bv)
{
    const wchar_t *a = *(const wchar_t *const *)av;
    const wchar_t *b = *(const wchar_t *const *)bv;
    bool adir = a[wcslen(a) - 1] == L'/', bdir = b[wcslen(b) - 1] == L'/';
    if (adir != bdir)
        return adir ? -1 : +1;
    return _wcsicmp(a, b);
}

static void list_done(void *ctx, SftpXferResult result, const char *text)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    ws->list_xfer = NULL;
    if (result != SFTP_XFER_OK) {
        for (size_t i = 0; i < ws->n_list; i++)
            sfree(ws->list_names[i]);
        ws->n_list = 0;
    }
    qsort(ws->list_names, ws->n_list, sizeof(*ws->list_names), list_cmp);
    wui_term_listing(ws->tterm, ws->list_dir,
                     (const wchar_t *const *)ws->list_names, ws->n_list);
}

static const SftpXferClient list_client = {
    .entry = list_entry,
    .done = list_done,
};

void wui_cb_list(void *ctx, const wchar_t *dir)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    list_stop(ws);
    ws->list_dir = dupwcs(dir);
    char *udir = wide_to_utf8(dir);
    ws->list_xfer = ws->wgs.backend ?
        sftp_xfer_list(ws->wgs.backend, &list_client, ws, udir) : NULL;
    sfree(udir);
    if (!ws->list_xfer)
        wui_term_listing(ws->tterm, dir, NULL, 0);
}

/* ---- Sending ---- */

static void send_progress(void *ctx, uint64_t sofar, uint64_t total)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    wchar_t a[32], b[32], text[80];
    swprintf(text, lenof(text), L"%ls of %ls",
             size_text(a, lenof(a), sofar), size_text(b, lenof(b), total));
    wui_term_send_state(ws->tterm, WUI_SEND_BUSY, text,
                        total ? (double)sofar / (double)total : 1.0);
}

static void send_done(void *ctx, SftpXferResult result, const char *text)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    wchar_t *wtext = utf8_to_wide(text);
    wchar_t *msg;
    ws->xfer = NULL;
    switch (result) {
      case SFTP_XFER_OK:
        msg = dupwcscat(L"Sent to ", wtext);
        wui_term_send_state(ws->tterm, WUI_SEND_DONE, msg, 1.0);
        break;
      case SFTP_XFER_EXISTS:
        msg = dupwcscat(wtext, L" already exists.");
        wui_term_send_state(ws->tterm, WUI_SEND_EXISTS, msg, -1);
        break;
      default:
        msg = dupwcscat(wtext);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR, msg, -1);
        break;
    }
    sfree(msg);
    sfree(wtext);
}

static const SftpXferClient send_client = {
    .progress = send_progress,
    .done = send_done,
};

/* The Send File command: pick a file, and show the bar for it */
static void send_file(WuiSeat *ws)
{
    WinGuiSeat *wgs = &ws->wgs;
    if (!wgs->backend || !sftp_xfer_available(wgs->backend) ||
        xfer_busy(ws))
        return;                        /* (one at a time) */

    wchar_t *path = file_dialog(false, L"Send File", &send_file_dialog,
                                NULL);
    if (!path)
        return;
    if (ws->send_file)
        filename_free(ws->send_file);
    ws->send_file = filename_from_wstr(path);
    const wchar_t *name = wcsrchr(path, L'\\');
    xfer_bar_start(ws, false, name ? name + 1 : path);
    sfree(path);
}

void wui_cb_send(void *ctx, const wchar_t *dest, bool overwrite)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;
    if (!ws->send_file || xfer_busy(ws))
        return;
    xfer_stop(ws);                     /* (finding home: never mind) */

    FILE *fp = f_open(ws->send_file, "rb", false);
    if (!fp) {
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR,
                            L"Couldn't open the file", -1);
        return;
    }
    _fseeki64(fp, 0, SEEK_END);
    uint64_t size = (uint64_t)_ftelli64(fp);
    _fseeki64(fp, 0, SEEK_SET);

    char *dir = wide_to_utf8(dest);
    const char *name = path_leaf(ws->send_file->utf8path, "\\/:");
    ws->xfer_kind = XK_SEND;
    ws->xfer = wgs->backend ?
        sftp_xfer_send(wgs->backend, &send_client, ws, dir, name, fp, size,
                       overwrite) : NULL;
    if (!ws->xfer) {
        if (!wgs->backend)
            fclose(fp);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR,
                            L"The session isn't connected", -1);
    } else {
        sfree(ws->send_dir);
        ws->send_dir = dupstr(dir);
        send_recent_add(dest);
        wui_term_send_state(ws->tterm, WUI_SEND_BUSY, L"Starting...", 0);
    }
    sfree(dir);
}

/* ---- Receiving ---- */

static void receive_file(WuiSeat *ws)
{
    WinGuiSeat *wgs = &ws->wgs;
    if (!wgs->backend || !sftp_xfer_available(wgs->backend) ||
        xfer_busy(ws))
        return;                        /* (one at a time) */
    xfer_bar_start(ws, true, NULL);
}

static void receive_progress(void *ctx, uint64_t sofar, uint64_t total)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    wchar_t a[32], b[32], text[80];
    if (total)
        swprintf(text, lenof(text), L"%ls of %ls",
                 size_text(a, lenof(a), sofar), size_text(b, lenof(b), total));
    else
        size_text(text, lenof(text), sofar);
    wui_term_send_state(ws->tterm, WUI_SEND_BUSY, text,
                        total ? (double)min(sofar, total) / (double)total :
                        -1);
}

static void receive_done(void *ctx, SftpXferResult result, const char *text)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    ws->xfer = NULL;

    if (result == SFTP_XFER_OK) {
        /* (it went to a file of its own, so as not to lose the one
         * it's replacing until it's all here) */
        if (!MoveFileExW(ws->recv_part, ws->recv_local,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            DeleteFileW(ws->recv_part);
            wchar_t *msg = dupwcscat(L"Couldn't save it as ",
                                     ws->recv_local);
            wui_term_send_state(ws->tterm, WUI_SEND_ERROR, msg, -1);
            sfree(msg);
            return;
        }
        /* (next time, start from where it came from) */
        sfree(ws->recv_dir);
        ws->recv_dir = dupstr(ws->recv_path);
        *(char *)path_leaf(ws->recv_dir, "/") = '\0';
        wchar_t *msg = dupwcscat(L"Saved to ", ws->recv_local);
        wui_term_send_state(ws->tterm, WUI_SEND_DONE, msg, 1.0);
        sfree(msg);
    } else {
        DeleteFileW(ws->recv_part);
        wchar_t *msg = utf8_to_wide(text);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR, msg, -1);
        sfree(msg);
    }
}

static const SftpXferClient receive_client = {
    .progress = receive_progress,
    .done = receive_done,
};

/* Once we know the file's there: ask where to put it, and get it.
 * (From a toplevel callback, since the dialog runs a message loop of
 * its own, which mustn't be in the middle of the SSH code.) */
static void receive_ask(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;
    if (!ws->recv_asking)
        return;                        /* (the bar's been closed) */

    wchar_t *name = utf8_to_wide(path_leaf(ws->recv_path, "/"));
    wchar_t *local = file_dialog(true, L"Receive File",
                                 &receive_file_dialog, name);
    sfree(name);
    ws->recv_asking = false;
    if (!local) {
        wui_term_send_state(ws->tterm, WUI_SEND_READY, L"", -1);
        return;
    }

    sfree(ws->recv_local);
    sfree(ws->recv_part);
    ws->recv_local = local;
    ws->recv_part = dupwcscat(local, L".part");
    FILE *fp = _wfopen(ws->recv_part, L"wb");
    if (!fp) {
        wchar_t *msg = dupwcscat(L"Couldn't create ", ws->recv_part);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR, msg, -1);
        sfree(msg);
        return;
    }
    ws->xfer_kind = XK_RECEIVE;
    ws->xfer = wgs->backend ?
        sftp_xfer_receive(wgs->backend, &receive_client, ws,
                          ws->recv_path, fp) : NULL;
    if (!ws->xfer) {
        if (!wgs->backend)
            fclose(fp);
        DeleteFileW(ws->recv_part);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR,
                            L"The session isn't connected", -1);
        return;
    }
    wui_term_send_state(ws->tterm, WUI_SEND_BUSY, L"Starting...", -1);
}

static void check_done(void *ctx, SftpXferResult result, const char *text)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    ws->xfer = NULL;
    if (result != SFTP_XFER_OK) {
        wchar_t *msg = utf8_to_wide(text);
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR, msg, -1);
        sfree(msg);
        return;
    }
    sfree(ws->recv_path);
    ws->recv_path = dupstr(text);
    ws->recv_asking = true;
    queue_toplevel_callback(receive_ask, ws);
}

static const SftpXferClient check_client = {
    .done = check_done,
};

void wui_cb_receive(void *ctx, const wchar_t *path)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;
    if (xfer_busy(ws))
        return;
    xfer_stop(ws);                     /* (finding home: never mind) */

    char *upath = wide_to_utf8(path);
    ws->xfer_kind = XK_CHECK;
    ws->xfer = wgs->backend ?
        sftp_xfer_check(wgs->backend, &check_client, ws, upath) : NULL;
    sfree(upath);
    if (!ws->xfer)
        wui_term_send_state(ws->tterm, WUI_SEND_ERROR,
                            L"The session isn't connected", -1);
    else
        wui_term_send_state(ws->tterm, WUI_SEND_BUSY, L"Looking for it...",
                            -1);
}

/* Show the file that was received, in Explorer */
void wui_cb_show_received(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    if (!ws->recv_local)
        return;
    wchar_t *params = dupwcscat(L"/select,\"", ws->recv_local, L"\"");
    ShellExecuteW(NULL, L"open", L"explorer.exe", params, NULL,
                  SW_SHOWNORMAL);
    sfree(params);
}

void wui_cb_char(void *ctx, wchar_t c)
{
    WinGuiSeat *wgs = &((WuiSeat *)ctx)->wgs;

    if (!wgs->term)
        return;

    if (IS_HIGH_SURROGATE(c)) {
        wgs->pending_surrogate = c;
    } else if (IS_SURROGATE_PAIR(wgs->pending_surrogate, c)) {
        wchar_t pair[2];
        pair[0] = wgs->pending_surrogate;
        pair[1] = c;
        term_keyinputw(wgs->term, pair, 2);
    } else if (!IS_SURROGATE(c)) {
        term_keyinputw(wgs->term, &c, 1);
    }
}

/* ----------------------------------------------------------------------
 * Subclass of the XAML window's HWND, for the things the XAML APIs
 * don't give us: the window menu on the title bar, interactive resizing
 * in character cell steps, and the size tip.
 */

static bool sizing(WinGuiSeat *wgs, WPARAM wParam, RECT *r);

/* Is a point (in screen coordinates) on the title bar? */
static bool on_title_bar(HWND hwnd, LPARAM pos)
{
    LRESULT hit = SendMessageW(hwnd, WM_NCHITTEST, 0, pos);
    return hit == HTCAPTION || hit == HTSYSMENU;
}

static LRESULT CALLBACK TermWndProc(HWND hwnd, UINT message,
                                    WPARAM wParam, LPARAM lParam)
{
    WuiSeat *ws;

    switch (message) {
      /*
       * Windows' own system menu never shows: the window icon's menu
       * (see wui_show_window_menu) does instead, for right-clicking the
       * title bar, or Alt+Space.
       */
      case WM_SYSCOMMAND:
        if (active && ((wParam & ~0xF) == SC_KEYMENU ||
                       (wParam & ~0xF) == SC_MOUSEMENU))
            show_mouseptr(&active->wgs, true); /* make sure it's visible */
        if (host && (wParam & ~0xF) == SC_KEYMENU && lParam == ' ') {
            wui_show_window_menu(host, false);
            return 0;
        }
        if (host && (wParam & ~0xF) == SC_MOUSEMENU) {
            wui_show_window_menu(host, true);
            return 0;
        }
        break;
      case WM_NCRBUTTONUP:
        if (host && (wParam == HTCAPTION || wParam == HTSYSMENU)) {
            wui_show_window_menu(host, true);
            return 0;
        }
        break;
      case WM_NCRBUTTONDOWN:
        if (wParam == HTCAPTION || wParam == HTSYSMENU)
            return 0;
        break;
      case WM_CONTEXTMENU:
        if (host && lParam != -1 && on_title_bar(hwnd, lParam)) {
            wui_show_window_menu(host, true);
            return 0;
        }
        break;
      case WM_ENTERSIZEMOVE:
        EnableSizeTip(true);
        FOR_EACH_SEAT(ws) {
            ws->wgs.resizing = true;
            ws->wgs.need_backend_resize = false;
        }
        break;
      case WM_EXITSIZEMOVE:
        EnableSizeTip(false);
        FOR_EACH_SEAT(ws) {
            WinGuiSeat *wgs = &ws->wgs;
            wgs->resizing = false;
            if (wgs->need_backend_resize) {
                term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
                          conf_get_int(wgs->conf, CONF_width),
                          conf_get_int(wgs->conf, CONF_savelines));
                invalidate_all(wgs);
            }
            /* (and what handle_size left for now, unmaximising during
             * the drag) */
            if (ws->unzoom_pending) {
                ws->unzoom_pending = false;
                reset_window(wgs, conf_get_int(wgs->conf, CONF_resize_action)
                             == RESIZE_FONT ? 0 : 2);
            }
            recompute_window_offset(wgs);
        }
        break;
      case WM_SIZING:
        /* The size tip and the stepping follow the visible session */
        if (active && sizing(&active->wgs, wParam, (RECT *)lParam))
            return 1;
        break;
      case WM_MOVE:
        FOR_EACH_SEAT(ws) {
            term_notify_window_pos(ws->wgs.term, (short)LOWORD(lParam),
                                   (short)HIWORD(lParam));
        }
        if (active)
            sys_cursor_update(&active->wgs);
        break;
      case WM_SYSCOLORCHANGE:
        FOR_EACH_SEAT(ws) {
            WinGuiSeat *wgs = &ws->wgs;
            if (conf_get_bool(wgs->conf, CONF_system_colour)) {
                /* Refresh palette from system colours. */
                term_notify_palette_changed(wgs->term);
                init_palette(wgs);
                /* Force a repaint of the terminal window. */
                term_invalidate(wgs->term);
            }
        }
        break;
    }

    return CallWindowProcW(old_wndproc, hwnd, message, wParam, lParam);
}

/* ----------------------------------------------------------------------
 * Startup.
 */

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    HRESULT hr;

    dll_hijacking_protection();
    enable_dit();

    hinst = inst;

    /* (to SSH servers, it's itself, not PuTTY: "SSH-2.0-JoliPuTTY_0.2") */
    ssh_client_name = "JoliPuTTY";
    ssh_client_version = "-" JOLIPUTTY_VERSION;

    sk_init();

    init_common_controls();

    /* Set Explicit App User Model Id so that jump lists don't cause
       PuTTY to hang on to removable media. */
    /* (unless the MSIX package has given us its own, which the taskbar
     * and the Start menu know us by) */

    if (!wui_packaged())
        set_explicit_app_user_model_id();

    /* Ensure a Maximize setting in Explorer doesn't maximise the
     * config box. */
    defuse_showwindow();

    init_winver();

    init_winfuncs();

    setup_gui_timing();

    show_cmd = show;

    /*
     * Initialize COM. The XAML framework needs a single-threaded
     * apartment, which is what this gives us.
     */
    hr = CoInitialize(NULL);
    if (hr != S_OK && hr != S_FALSE) {
        char *str = dupprintf("%s Fatal Error", appname);
        MessageBox(NULL, "Failed to initialize COM subsystem",
                   str, MB_OK | MB_ICONEXCLAMATION);
        sfree(str);
        return 1;
    }

    create_msg_window();
    winselgui_set_hwnd(msg_hwnd);
    request_callback_notifications(callback_notify, NULL);
    handle_wait_set_hook(handle_wait_hook, NULL);

    /*
     * Process the command line.
     * (If the command line doesn't provide enough info to start a
     * session, this will detour via the config box.)
     */
    initial_conf = conf_new();
    gui_term_process_cmdline(initial_conf, cmdline);

    /*
     * Now hand over to the XAML framework, which will call
     * wui_cb_launched once our window exists.
     */
    int ret = wui_run();

    cleanup_exit(exit_code ? exit_code : ret);  /* this doesn't return... */
    return ret;                        /* ... but optimiser doesn't know */
}

/*
 * Settings that belong to the window as a whole come from whichever
 * session is in the visible tab.
 */
static void apply_window_settings(WinGuiSeat *wgs)
{
    const struct BackendVtable *vt =
        backend_vt_from_proto(be_default_protocol);
    bool resize_forbidden = (vt && vt->flags & BACKEND_RESIZE_FORBIDDEN);
    bool resizable = !(resize_forbidden ||
                       conf_get_int(wgs->conf, CONF_resize_action) ==
                       RESIZE_DISABLED);
    wui_set_resizable(host, resizable, resizable);
    wui_set_topmost(host, conf_get_bool(wgs->conf, CONF_alwaysontop));
    wui_app_theme_changed();
}

void wui_app_theme_changed(void)
{
    if (!host)
        return;
    int theme = app_theme_get();
    wui_set_theme(host, app_theme_base(theme), app_theme_tinted_tab(theme));
    WuiSeat *ws;
    FOR_EACH_SEAT(ws)
        if (ws->log)
            wui_log_set_theme(ws->log, app_theme_base(theme));
}

static void update_window_title(void)
{
    if (!active)
        return;
    WinGuiSeat *wgs = &active->wgs;
    if (win_state == WUI_STATE_MINIMISED &&
        !conf_get_bool(wgs->conf, CONF_win_name_always))
        wui_set_title(host, wgs->icon_name);
    else
        wui_set_title(host, wgs->window_name);
}

void wui_cb_launched(WuiHost *h)
{
    host = h;
    win_state = wui_get_state(host);

    HWND hwnd = wui_hwnd(host);
    old_wndproc = (WNDPROC)SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, (LONG_PTR)TermWndProc);

    {
        HICON icon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON));
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        wui_set_icon(host, icon);
    }

    get_sesslist(&sesslist, true);

    launch_first_session(startup_config);
}

/*
 * The command line didn't say what to connect to, so rather than
 * asking with the config box (as the classic front end does), the
 * window opens with just the welcome tab in it.
 */
bool wui_defer_config(Conf *conf)
{
    startup_config = true;
    return true;
}

void wui_cleanup_all(void)
{
    autologon_cred_delete_all();
}

wchar_t *wui_help_url(const char *page)
{
    char *url = dupcat(JOLIPUTTY_HELP, page ? page : "");
    wchar_t *wurl = dup_mb_to_wc(DEFAULT_CODEPAGE, url);
    sfree(url);
    return wurl;
}

void wui_show_help(HWND hwnd, const char *page)
{
    wchar_t *url = wui_help_url(page);
    ShellExecuteW(hwnd, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    sfree(url);
}

static WuiSeat *open_welcome(Conf *conf);

static void launch_first_session(bool welcome)
{
    HWND hwnd = wui_hwnd(host);
    WuiSeat *ws = welcome ? open_welcome(initial_conf) :
        open_session(initial_conf);
    initial_conf = NULL;               /* now owned by the session */
    if (welcome) {
        sfree(initial_session);
    } else {
        set_session_name(ws, initial_session);
        session_touch(ws->session_name);
    }
    initial_session = NULL;
    WinGuiSeat *wgs = &ws->wgs;

    if (restricted_acl()) {
        lp_eventlog(&wgs->logpolicy, "Running with restricted process ACL");
    }

    key_hook = SetWindowsHookEx(WH_GETMESSAGE, GetMsgHookProc, NULL,
                                GetCurrentThreadId());

    /*
     * Finally show the window!
     */
    if (show_cmd == SW_SHOWMAXIMIZED || show_cmd == SW_MAXIMIZE)
        wui_set_state(host, WUI_STATE_MAXIMISED);
    else if (show_cmd == SW_SHOWMINIMIZED ||
             show_cmd == SW_MINIMIZE ||
             show_cmd == SW_SHOWMINNOACTIVE)
        wui_set_state(host, WUI_STATE_MINIMISED);
    wui_show(host);
    wui_focus_terminal(ws->tterm);
    SetForegroundWindow(hwnd);

    gui_terminal_ready(hwnd, &wgs->seat, wgs->backend);

    callbacks_enabled = true;
    if (toplevel_callback_pending())
        post_callback_msg();
}

/*
 * Start a session in a new tab. The first one also decides the size
 * of the window; later ones fit into the space there is.
 */
/* Where open_session should put the next session: in a new pane
 * beside this one's terminal, if it's set (see split_session) */
static WuiTerm *split_beside;
static bool split_side_by_side;
/* ... and whether it's the welcome tab (see open_welcome) */
static bool opening_welcome;

/*
 * The welcome tab, which the window opens with when there's nothing
 * to connect to: a terminal with no session in it, just a greeting.
 * Whatever session is opened next replaces it.
 */
static void show_welcome(WinGuiSeat *wgs)
{
    /* (as if a session had been and gone, so that closing it asks
     * nothing, and typing into it does nothing) */
    wgs->session_closed = true;
    win_set_title(&wgs->termwin, "Welcome", DEFAULT_CODEPAGE);
    win_set_icon_title(&wgs->termwin, "Welcome", DEFAULT_CODEPAGE);

    static const char *const logo[] = {
        "      _       _ _   ____        _____ _______   __",
        "     | | ___ | (_) |  _ \\ _   _|_   _|_   _\\ \\ / /",
        "  _  | |/ _ \\| | | | |_) | | | | | |   | |  \\ V / ",
        " | |_| | (_) | | | |  __/| |_| | | |   | |   | |  ",
        "  \\___/ \\___/|_|_| |_|    \\__,_| |_|   |_|   |_|  ",
    };
    /*
     * In the Atari ST's own colours (its palette had eight levels of
     * each of red, green and blue): the logo as a rainbow, a line of
     * it in each colour, and the title white on the ST's blue.
     */
    static const int colours[][3] = {
        { 255, 0, 0 }, { 255, 146, 0 }, { 255, 255, 0 },
        { 0, 255, 0 }, { 0, 182, 255 },
    };
    strbuf *sb = strbuf_new();
    put_fmt(sb, "\033[?25l\r\n");          /* (no cursor) */
    for (size_t i = 0; i < lenof(logo); i++)
        put_fmt(sb, "   \033[1;38;2;%d;%d;%dm%s\033[0m\r\n",
                colours[i][0], colours[i][1], colours[i][2], logo[i]);
    put_fmt(sb, "\r\n"
            "   \033[1;38;2;255;255;255;48;2;0;0;219m Welcome to JoliPuTTY "
            "\033[0m\r\n"
            "   \033[1;38;2;255;255;255mA playful fork of Simon Tatham's "
            "PuTTY\033[0m\r\n"
            "   \033[38;2;146;146;146mby Sylvain Cresto ;-)\033[0m\r\n"
            "\r\n"
            "   \033[38;2;255;255;0mSaved sessions\033[0m  the arrow beside "
            "the tabs, where you can\r\n"
            "                   also type a host name to connect to\r\n"
            "                   (user@host:port, telnet://host...)\r\n"
            "   \033[38;2;255;255;0mA new session\033[0m   the + button\r\n"
            "   \033[38;2;255;255;0mHelp\033[0m            the window's icon, "
            "top left\r\n");
    /* (not marked as PuTTY's own, which would put its icon at the
     * start of each line, as it does for what's said while logging in) */
    term_set_trust_status(wgs->term, false);
    term_data(wgs->term, sb->s, sb->len);
    strbuf_free(sb);
}

static WuiSeat *open_welcome(Conf *conf)
{
    opening_welcome = true;
    WuiSeat *ws = open_session(conf);
    opening_welcome = false;
    return ws;
}

/* A session's been opened, so the welcome tab can go */
static void close_welcome(void)
{
    WuiSeat *ws;
    FOR_EACH_SEAT(ws) {
        if (ws->welcome)
            queue_close_tab(&ws->wgs);
    }
}

/* ---- Pools of text to paste again ----
 *
 * Each tab keeps what's been copied in it lately (in memory only: not
 * all of that should be on the disk, such as passwords); and Copy to
 * Pool keeps something for the tab's saved session, or all sessions,
 * from one time to the next (in the registry: see pool_get). Ctrl+Alt+V,
 * or Paste from Pool, shows them all, to paste one.
 */

#define SAVED_POOL_MAX 50
#define POOL_ENTRY_MAX 16384           /* (characters) */

/* Put some text at the front of a pool, not twice, and not too many */
static void pool_add(wchar_t ***pool, size_t *n, size_t *size,
                     const wchar_t *text, size_t max)
{
    for (size_t i = 0; i < *n; i++)
        if (!wcscmp((*pool)[i], text)) {
            sfree((*pool)[i]);
            memmove(&(*pool)[i], &(*pool)[i + 1],
                    (*n - i - 1) * sizeof(**pool));
            (*n)--;
            break;
        }
    while (*n >= max)
        sfree((*pool)[--(*n)]);
    sgrowarray(*pool, *size, *n);
    memmove(&(*pool)[1], &(*pool)[0], *n * sizeof(**pool));
    (*pool)[0] = dupwcs(text);
    (*n)++;
}

/* ... and to a saved one */
static void saved_pool_add(const char *session, const wchar_t *text,
                           size_t max)
{
    size_t n, size;
    wchar_t **items = pool_get(session, &n);
    size = n;
    pool_add(&items, &n, &size, text, max);
    pool_set(session, items, n);
    pool_free(items, n);
}

static void saved_pool_remove(const char *session, const wchar_t *text)
{
    size_t n;
    wchar_t **items = pool_get(session, &n);
    for (size_t i = 0; i < n; i++)
        if (!wcscmp(items[i], text)) {
            sfree(items[i]);
            memmove(&items[i], &items[i + 1], (n - i - 1) * sizeof(*items));
            n--;
            break;
        }
    pool_set(session, items, n);
    pool_free(items, n);
}

/* Something copied (to the clipboard): into this tab's pool, and
 * where Copy to Pool said, if it was that */
static void pool_copied(WuiSeat *ws, const wchar_t *data, int len)
{
    size_t n = 0;
    while ((int)n < len && data[n])
        n++;
    int target = ws->pool_target;
    ws->pool_target = 0;
    /* (not Copy All, nor anything else as big as that's likely to be:
     * the pools are for things to type again, not screenfuls) */
    if (!n || ws->welcome || ws->pool_skip || n > POOL_ENTRY_MAX)
        return;
    /* (as many as the session says to keep: Terminal, Pools of copied
     * text; with none, nothing, and nothing kept from before either) */
    int keep = conf_get_int(ws->wgs.conf, CONF_copy_pool_size);
    if (keep <= 0) {
        pool_free(ws->pool, ws->npool);
        ws->pool = NULL;
        ws->npool = ws->poolsize = 0;
        return;
    }
    wchar_t *text = snewn(n + 1, wchar_t);
    memcpy(text, data, n * sizeof(wchar_t));
    text[n] = L'\0';
    pool_add(&ws->pool, &ws->npool, &ws->poolsize, text, keep);
    if (target == WUI_POOL_SESSION && ws->session_name)
        saved_pool_add(ws->session_name, text, keep);
    else if (target == WUI_POOL_COMMON)
        saved_pool_add(NULL, text, SAVED_POOL_MAX);
    sfree(text);
}

static void forget_shown_pool(WuiSeat *ws)
{
    for (size_t i = 0; i < ws->nshown; i++)
        sfree((wchar_t *)ws->shown[i].text);
    sfree(ws->shown);
    ws->shown = NULL;
    ws->nshown = 0;
}

/* The pools' pop-up, at the cursor */
static void show_pool(WuiSeat *ws)
{
    if (!ws->tterm || ws->welcome)
        return;
    forget_shown_pool(ws);
    size_t ns = 0, nc = 0;
    wchar_t **sp = ws->session_name ? pool_get(ws->session_name, &ns) : NULL;
    wchar_t **cp = pool_get(NULL, &nc);
    ws->shown = snewn(ws->npool + ns + nc + 1, WuiPoolItem);
    for (size_t i = 0; i < ws->npool; i++)
        ws->shown[ws->nshown++] = (WuiPoolItem){
            WUI_POOL_TAB, dupwcs(ws->pool[i]) };
    for (size_t i = 0; i < ns; i++)
        ws->shown[ws->nshown++] = (WuiPoolItem){
            WUI_POOL_SESSION, dupwcs(sp[i]) };
    for (size_t i = 0; i < nc; i++)
        ws->shown[ws->nshown++] = (WuiPoolItem){
            WUI_POOL_COMMON, dupwcs(cp[i]) };
    pool_free(sp, ns);
    pool_free(cp, nc);
    WinGuiSeat *wgs = &ws->wgs;
    int x = wgs->caret_x >= 0 ? wgs->caret_x : 0;
    int y = wgs->caret_y >= 0 ? wgs->caret_y + wgs->font_height : 0;
    wui_term_show_pool(ws->tterm, x, y, ws->shown, ws->nshown,
                       ws->session_name != NULL);
}

void wui_cb_pool_action(void *ctx, int index, int action)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;
    if (action == WUI_POOL_CLEAR_TAB) {
        pool_free(ws->pool, ws->npool);
        ws->pool = NULL;
        ws->npool = ws->poolsize = 0;
        return;
    }
    if (index < 0 || (size_t)index >= ws->nshown)
        return;
    const WuiPoolItem *it = &ws->shown[index];
    switch (action) {
      case WUI_POOL_PASTE:
        if (wgs->term)
            term_do_paste(wgs->term, it->text, wcslen(it->text));
        break;
      case WUI_POOL_KEEP_SESSION:
        if (ws->session_name &&
            conf_get_int(wgs->conf, CONF_copy_pool_size) > 0)
            saved_pool_add(ws->session_name, it->text,
                           conf_get_int(wgs->conf, CONF_copy_pool_size));
        break;
      case WUI_POOL_KEEP_COMMON:
        saved_pool_add(NULL, it->text, SAVED_POOL_MAX);
        break;
      case WUI_POOL_REMOVE:
        if (it->pool == WUI_POOL_TAB) {
            for (size_t i = 0; i < ws->npool; i++)
                if (!wcscmp(ws->pool[i], it->text)) {
                    sfree(ws->pool[i]);
                    memmove(&ws->pool[i], &ws->pool[i + 1],
                            (ws->npool - i - 1) * sizeof(*ws->pool));
                    ws->npool--;
                    break;
                }
        } else {
            saved_pool_remove(it->pool == WUI_POOL_SESSION ?
                              ws->session_name : NULL, it->text);
        }
        break;
    }
}

/* ---- The About box ---- */


static void show_about(HWND owner)
{
    char *title = dupprintf("About %s", appname);
    char *build = buildinfo("\n");
    wchar_t *wtitle = dup_mb_to_wc(DEFAULT_CODEPAGE, title);
    wchar_t *wname = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
    /* (this front end's own version, and which of PuTTY's it's from,
     * rather than PuTTY's "Unidentified build" of a build of its own;
     * though if PuTTY's build system named it, that too) */
    char *version = !strcmp(ver, "Unidentified build") ?
        dupprintf("Version %s, based on PuTTY %s", JOLIPUTTY_VERSION,
                  JOLIPUTTY_BASE) :
        dupprintf("Version %s, based on PuTTY %s (%s)", JOLIPUTTY_VERSION,
                  JOLIPUTTY_BASE, ver);
    wchar_t *wver = dup_mb_to_wc(DEFAULT_CODEPAGE, version);
    sfree(version);
    wchar_t *wbuild = dup_mb_to_wc(DEFAULT_CODEPAGE, build);
    /* (the copyright sign is in Latin-1, whatever code page this is) */
    wchar_t *worig = dup_mb_to_wc(1252, about_copyright());
    wchar_t *wcopy = dupwcscat(L"Based on PuTTY ", worig,
                               L" and contributors.");
    sfree(worig);
    wchar_t *wlicence = dup_mb_to_wc(DEFAULT_CODEPAGE, about_licence());
    WuiAbout about = {
        .title = wtitle,
        .name = wname,
        .version = wver,
        .byline = L"A playful fork of the original PuTTY ;-)",
        .build = wbuild,
        .copyright = wcopy,
        .licence = wlicence,
        .fork_copyright = L"JoliPuTTY \u00A9 2026 Sylvain Cresto",
        .notice = L"JoliPuTTY is an independent project, based on PuTTY. "
                  L"It is not affiliated with, or endorsed by, the PuTTY "
                  L"project or its authors.",
        .website = L"https://github.com/scresto09/joliputty/",
        .website_label = L"JoliPuTTY's web site",
    };
    wui_about(owner, LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON)),
              app_theme_base(app_theme_get()), &about);
    sfree(wlicence);
    sfree(wcopy);
    sfree(wbuild);
    sfree(wver);
    sfree(wname);
    sfree(wtitle);
    sfree(build);
    sfree(title);
}

/* ---- The Event Log window ---- */

/* Give a session's Event Log window all its events */
static void log_fill(WuiSeat *ws)
{
    int n = eventlog_count(&ws->wgs);
    wchar_t **lines = snewn(n ? n : 1, wchar_t *);
    for (int i = 0; i < n; i++)
        lines[i] = dup_mb_to_wc(DEFAULT_CODEPAGE, eventlog_get(&ws->wgs, i));
    wui_log_set(ws->log, n, (const wchar_t *const *)lines);
    for (int i = 0; i < n; i++)
        sfree(lines[i]);
    sfree(lines);
}

static void log_changed(WinGuiSeat *wgs)
{
    if (WUI(wgs)->log)
        log_fill(WUI(wgs));
}

static void show_log(WuiSeat *ws)
{
    if (!ws->log) {
        wchar_t *title = dupwcscat(ws->wgs.window_name ?
                                   ws->wgs.window_name : WUI_APP_NAME,
                                   L" - Event Log");
        ws->log = wui_log_new(host ? wui_hwnd(host) : NULL, title,
                              LoadIcon(hinst, MAKEINTRESOURCE(IDI_MAINICON)),
                              app_theme_base(app_theme_get()), ws);
        sfree(title);
        log_fill(ws);
    }
    wui_log_show(ws->log);
}

void wui_log_cb_closed(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    wui_log_free(ws->log);
    ws->log = NULL;
}

static WuiSeat *open_session(Conf *conf)
{
    bool first = (nseats == 0);

    WuiSeat *ws = snew(WuiSeat);
    memset(ws, 0, sizeof(*ws));
    ws->id = ++seat_serial;
    ws->trust_icon = INVALID_HANDLE_VALUE;
    ws->welcome = opening_welcome;
    WinGuiSeat *wgs = &ws->wgs;
    wgs_link(wgs);
    nseats++;

    wgs->seat.vt = &win_seat_vt;
    wgs->logpolicy.vt = &win_gui_logpolicy_vt;
    wgs->eventlog_changed = log_changed;
    wgs->termwin.vt = &winui_termwin_vt;

    wgs->caret_x = wgs->caret_y = -1;
    wgs->busy_status = BUSY_NOT;
    wgs->conf = conf;
    memset(&wgs->ucsdata, 0, sizeof(wgs->ucsdata));
    conf_cache_data(wgs);

    wgs->term_hwnd = wui_hwnd(host);
    wgs->window_name = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
    wgs->icon_name = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);

    ws->tterm = split_beside ?
        wui_term_new_split(host, ws, split_beside, split_side_by_side) :
        wui_term_new(host, ws);
    split_beside = NULL;
    wui_term_set_title(ws->tterm, wgs->window_name);
    active = ws;
    apply_window_settings(wgs);
    update_window_title();

    memset(&wgs->dpi_info, 0, sizeof(struct _dpi_info));
    wgs->dpi_info.cur_dpi.x = wgs->dpi_info.cur_dpi.y = wui_get_dpi(host);

    /*
     * Initialise the fonts, simultaneously correcting the guesses
     * for font_{width,height}.
     */
    init_fonts(wgs, 0, 0);

    /*
     * Prepare a palette.
     */
    init_palette(wgs);

    /*
     * Initialise the terminal.
     */
    wgs->term = term_init(wgs->conf, &wgs->ucsdata, &wgs->termwin);
    setup_clipboards(wgs->term, wgs->conf);
    wgs->logctx = log_init(&wgs->logpolicy, wgs->conf);
    term_provide_logctx(wgs->term, wgs->logctx);
    term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
              conf_get_int(wgs->conf, CONF_width),
              conf_get_int(wgs->conf, CONF_savelines));

    update_scrollbar_visibility(wgs);

    wgs->offset_width = wgs->offset_height =
        conf_get_int(wgs->conf, CONF_window_border);

    if (first) {
        /*
         * Size the terminal area to fit the terminal, clipped to the
         * screen, and make sure the window lies within the monitor's
         * working area.
         */
        int pw = wgs->font_width * wgs->term->cols + 2 * wgs->offset_width;
        int ph = wgs->font_height * wgs->term->rows + 2 * wgs->offset_height;
        RECT war;
        wui_set_panel_size(ws->tterm, pw, ph);

        if (get_workingarea_rect(wgs, &war)) {
            RECT winr;
            GetWindowRect(wgs->term_hwnd, &winr);
            int ww = winr.right - winr.left, wh = winr.bottom - winr.top;
            int x = winr.left, y = winr.top;

            if (ww > war.right - war.left || wh > war.bottom - war.top) {
                /* Too big for the screen: shrink the terminal area, and
                 * reset_window will sort out the terminal size later */
                int ew = ww - pw, eh = wh - ph;
                if (ww > war.right - war.left)
                    pw = war.right - war.left - ew;
                if (wh > war.bottom - war.top)
                    ph = war.bottom - war.top - eh;
                wui_set_panel_size(ws->tterm, pw, ph);
                GetWindowRect(wgs->term_hwnd, &winr);
                ww = winr.right - winr.left;
                wh = winr.bottom - winr.top;
            }

            /*
             * Move it left if it overlaps off the right side, then
             * right if it overlaps off the left side, preferring to
             * keep the top left corner on screen. Similarly in y.
             */
            if (x + ww > war.right)
                x = war.right - ww;
            if (x < war.left)
                x = war.left;
            if (y + wh > war.bottom)
                y = war.bottom - wh;
            if (y < war.top)
                y = war.top;
            SetWindowPos(wgs->term_hwnd, NULL, x, y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    {
        RECT cr, wr;
        int pw, ph;
        GetWindowRect(wgs->term_hwnd, &wr);
        wui_get_panel_size(ws->tterm, &pw, &ph);
        cr.left = cr.top = 0;
        cr.right = pw;
        cr.bottom = ph;
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;
    }

    /*
     * Set up a caret bitmap, with no content. The caret is never
     * visible, but we maintain one for the benefit of screen readers
     * and other accessibility software that tracks it.
     */
    {
        char *bits;
        int size = (wgs->font_width + 15) / 16 * 2 * wgs->font_height;
        bits = snewn(size, char);
        memset(bits, 0, size);
        wgs->caretbm = CreateBitmap(wgs->font_width, wgs->font_height,
                                    1, 1, bits);
        sfree(bits);
    }

    /*
     * Initialise the scroll bar.
     */
    wui_set_scrollbar(ws->tterm, wgs->term->rows, 0, wgs->term->rows);

    /*
     * Prepare the mouse handler.
     */
    wgs->lastact = MA_NOTHING;
    wgs->lastbtn = MBT_NOTHING;
    wgs->dbltime = GetDoubleClickTime();

    if (ws->welcome) {
        show_welcome(wgs);
        wui_term_set_hint(ws->tterm, L"Double-click for a new session");
    } else {
        start_backend(wgs);
        close_welcome();
    }

    /*
     * Set up the initial input locale.
     */
    set_input_locale(wgs, GetKeyboardLayout(0));

    /*
     * A new tab joining an existing window has to make do with the
     * terminal area everyone else has.
     */
    if (!first) {
        int pw, ph;
        wui_get_panel_size(ws->tterm, &pw, &ph);
        handle_size(wgs, pw, ph);
        wui_focus_terminal(ws->tterm);
    }

    invalidate_all(wgs);
    return ws;
}

/*
 * Free everything belonging to a session. (Doesn't touch its tab.)
 */
static void free_session(WinGuiSeat *wgs)
{
    WuiSeat *ws = WUI(wgs);

    delete_callbacks_for_context(wgs);
    xfer_free(ws);
    sfree(ws->list_names);
    sfree(ws->session_name);
    pool_free(ws->pool, ws->npool);
    forget_shown_pool(ws);
    if (wgs->ldisc) {
        ldisc_free(wgs->ldisc);
        wgs->ldisc = NULL;
    }
    if (wgs->backend) {
        backend_free(wgs->backend);
        wgs->backend = NULL;
    }
    if (wgs->term) {
        term_free(wgs->term);
        wgs->term = NULL;
    }
    if (wgs->logctx) {
        log_free(wgs->logctx);
        wgs->logctx = NULL;
    }
    if (WUI(wgs)->log) {
        wui_log_free(WUI(wgs)->log);
        WUI(wgs)->log = NULL;
    }
    eventlog_free(wgs->eventlog);
    wgs->eventlog = NULL;
    autologon_stop(WUI(wgs));
    deinit_fonts(wgs);
    if (wgs->caretbm)
        DeleteObject(wgs->caretbm);
    sfree(wgs->logpal);
    sfree(wgs->window_name);
    sfree(wgs->icon_name);
    if (wgs->conf)
        conf_free(wgs->conf);
    wgs_unlink(wgs);
    nseats--;
    if (active == ws)
        active = NULL;
    sfree(ws);
}

/*
 * Close a session's tab. If it was the last one, that's the end of
 * the program.
 */
static void close_tab(WinGuiSeat *wgs)
{
    WuiSeat *ws = WUI(wgs);
    WuiTerm *tterm = ws->tterm;

    show_mouseptr(wgs, true);
    free_session(wgs);

    if (nseats == 0) {
        /* Leave the window up until the application goes away, so
         * that there's never an empty one on the screen */
        wui_exit();
        return;
    }

    /* Removing the tab selects another, which will tell us about it */
    wui_term_free(tterm);
}

static void close_tab_callback(void *vctx)
{
    /* Called with the seat's id, since the seat itself might have
     * gone by the time we get here */
    WuiSeat *ws = find_seat((unsigned)(uintptr_t)vctx);
    if (ws)
        close_tab(&ws->wgs);
}

static void queue_close_tab(WinGuiSeat *wgs)
{
    queue_toplevel_callback(close_tab_callback,
                            (void *)(uintptr_t)WUI(wgs)->id);
}

void wui_cb_tab_activated(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    if (ws == active)
        return;
    if (active)
        wui_cb_focus(active, false);
    active = ws;
    apply_window_settings(&ws->wgs);
    update_window_title();
    update_mouse_pointer(&ws->wgs);
    flash_window(&ws->wgs, 0);
}

void wui_cb_new_tab_requested(void)
{
    PostMessage(msg_hwnd, WM_WUI_COMMAND, IDM_NEWSESS, 0);
}

/* Double-clicking a tab changes its session's settings (or for the
 * welcome tab, which has none, makes a new one, as its '+' does) */
void wui_cb_tab_double_clicked(void *ctx)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    PostMessage(msg_hwnd, WM_WUI_COMMAND,
                ws->welcome ? IDM_NEWSESS : IDM_RECONF, ws->id);
}

void wui_cb_new_window_requested(void)
{
    PostMessage(msg_hwnd, WM_WUI_COMMAND, IDM_NEWWIN, 0);
}

void wui_cb_tab_close_requested(void *ctx)
{
    PostMessage(msg_hwnd, WM_WUI_CLOSE_TAB, ((WuiSeat *)ctx)->id, 0);
}

char *handle_restrict_acl_cmdline_prefix(char *p)
{
    /*
     * Process the &R prefix on a command line, which is equivalent to
     * -restrict-acl but lexically easier to prepend when another
     * instance of ourself automatically constructs a command line.
     *
     * If successful, restricts the process ACL and advances the input
     * pointer past the prefix. Returns the updated pointer (whether
     * it moved or not).
     */
    while (*p && isspace((unsigned char)*p))
        p++;
    if (*p == '&' && p[1] == 'R' &&
        (!p[2] || p[2] == '@' || p[2] == '&')) {
        /* &R restrict-acl prefix */
        restrict_process_acl();
        p += 2;
    }
    return p;
}

bool handle_special_sessionname_cmdline(char *p, Conf *conf)
{
    /*
     * Process the special form of command line with an initial @
     * followed by the name of a saved session with _no quoting or
     * escaping_. This is a very convenient means of automated
     * saved-session launching, via IDM_SAVEDSESS or Windows 7 jump
     * lists.
     *
     * If successful, the whole command line has been interpreted in
     * this way, so there's nothing left to parse into other arguments.
     */
    if (*p != '@')
        return false;

    ptrlen sessionname = ptrlen_from_asciz(p + 1);
    while (sessionname.len > 0 &&
           isspace(((unsigned char *)sessionname.ptr)[sessionname.len-1]))
        sessionname.len--;

    char *dup = mkstr(sessionname);
    bool loaded = do_defaults(dup, conf);
    if (loaded) {
        sfree(initial_session);
        initial_session = dup;
    } else {
        sfree(dup);
    }

    return loaded;
}

bool handle_special_filemapping_cmdline(char *p, Conf *conf)
{
    /*
     * Process the special form of command line with an initial &
     * followed by the hex value of a HANDLE for a file mapping object
     * and the size of the data contained in it, which we must
     * interpret as a serialised Conf.
     *
     * If successful, the whole command line has been interpreted in
     * this way, so there's nothing left to parse into other arguments.
     */

    if (*p != '&')
        return false;

    HANDLE filemap;
    unsigned cpsize;
    if (sscanf(p + 1, "%p:%u", &filemap, &cpsize) != 2)
        return false;

    void *cp = MapViewOfFile(filemap, FILE_MAP_READ, 0, 0, cpsize);
    if (!cp)
        return false;

    BinarySource src[1];
    BinarySource_BARE_INIT(src, cp, cpsize);
    if (!conf_deserialise(conf, src))
        modalfatalbox("Serialised configuration data was invalid");
    UnmapViewOfFile(cp);
    CloseHandle(filemap);
    return true;
}

static void setup_clipboards(Terminal *term, Conf *conf)
{
    assert(term->mouse_select_clipboards[0] == CLIP_LOCAL);

    term->n_mouse_select_clipboards = 1;

    if (conf_get_bool(conf, CONF_mouseautocopy)) {
        term->mouse_select_clipboards[
            term->n_mouse_select_clipboards++] = CLIP_SYSTEM;
    }

    switch (conf_get_int(conf, CONF_mousepaste)) {
      case CLIPUI_IMPLICIT:
        term->mouse_paste_clipboard = CLIP_LOCAL;
        break;
      case CLIPUI_EXPLICIT:
        term->mouse_paste_clipboard = CLIP_SYSTEM;
        break;
      default:
        term->mouse_paste_clipboard = CLIP_NULL;
        break;
    }
}

/*
 * Clean up and exit.
 */
void cleanup_exit(int code)
{
    /*
     * Clean up.
     */
    if (key_hook) {
        UnhookWindowsHookEx(key_hook);
        key_hook = NULL;
    }
    while (wgslisthead.next != &wgslisthead) {
        WinGuiSeat *wgs = container_of(
            wgslisthead.next, WinGuiSeat, wgslistnode);
        free_session(wgs);
    }
    sk_cleanup();

    random_save_seed();

    /* Clean up COM. */
    CoUninitialize();

    exit(code);
}

/* ----------------------------------------------------------------------
 * Menus: the items of the XAML menus (the terminal's, the tab's and
 * the window icon's), for the session they're opened on.
 */

static const wchar_t *menu_string(wchar_t *s)
{
    sgrowarray(menu_strings, menu_strings_size, n_menu_strings);
    menu_strings[n_menu_strings++] = s;
    return s;
}

static void menu_add(WuiMenuKind kind, unsigned id,
                     const wchar_t *label, bool enabled, bool checked)
{
    sgrowarray(menu, menu_size, menu_len);
    WuiMenuItem *item = &menu[menu_len++];
    item->kind = kind;
    item->id = id;
    item->label = label;
    item->enabled = enabled;
    item->checked = checked;
    item->icon = 0;
    item->primary = false;
    item->accel = NULL;
}

/* ... and the keyboard shortcut that does what it does */
static void menu_set_accel(const wchar_t *accel)
{
    menu[menu_len - 1].accel = accel;
}

/* ... or one of JoliPuTTY's own, which the session may have left to the
 * server instead (see handle_key_message) */
static bool menu_own_accels;
static void menu_set_own_accel(const wchar_t *accel)
{
    if (menu_own_accels)
        menu_set_accel(accel);
}

/* Give the item just added an icon, in the context menu (the system
 * menu has none) */
static void menu_set_icon(unsigned glyph)
{
    menu[menu_len - 1].icon = glyph;
}

/* Segoe Fluent Icons glyphs */
#define ICON_COPY        0xE8C8
#define ICON_PASTE       0xE77F
#define ICON_COMMAND     0xE756
#define ICON_HISTORY     0xE81C
#define ICON_ADD         0xE710
#define ICON_NEWWINDOW   0xE78B
#define ICON_REFRESH     0xE72C
#define ICON_FAVOURITES  0xE728
#define ICON_SETTINGS    0xE713
#define ICON_SELECTALL   0xE8B3
#define ICON_DELETE      0xE74D
#define ICON_FULLSCREEN  0xE740
#define ICON_INFO        0xE946
#define ICON_HELP        0xE897
#define ICON_BACK        0xE72B
#define ICON_FORWARD     0xE72A
#define ICON_CLOSE       0xE711
#define ICON_FIND        0xE721
#define ICON_UPLOAD      0xE898
#define ICON_DOWNLOAD    0xE896
#define ICON_SPLITRIGHT  0xE90D
#define ICON_SPLITDOWN   0xE90E
#define ICON_CLOSEPANE   0xE89F
/* (as for the Session category of the configuration box) */
#define ICON_SESSION     0xE80F
#define ICON_RENAME      0xE8AC
#define ICON_POOL        0xE81C        /* (History) */

#define MENU_ITEM(id, label) \
    menu_add(WUI_MENU_ITEM, id, label, true, false)
#define MENU_ICON_ITEM(id, label, glyph) \
    (MENU_ITEM(id, label), menu_set_icon(glyph))
/* An icon button along the top of the context menu */
#define MENU_PRIMARY_ITEM(id, label, glyph, enabled) \
    (menu_add(WUI_MENU_ITEM, id, label, enabled, false), \
     menu_set_icon(glyph), menu[menu_len - 1].primary = true)
#define MENU_SEP() menu_add(WUI_MENU_SEPARATOR, 0, NULL, false, false)

/*
 * The terminal's context menu: what's to do with the terminal itself.
 * (The tab's menu has what's to do with the session, and the window
 * icon's the rest: full screen, About.)
 */
static void build_split_items(WinGuiSeat *wgs);

static void build_menu(WinGuiSeat *wgs)
{
    for (size_t i = 0; i < n_menu_strings; i++)
        sfree(menu_strings[i]);
    n_menu_strings = 0;
    menu_len = 0;
    menu_own_accels = !conf_get_bool(wgs->conf, CONF_no_wui_shortcuts);

    /* Grey out Copy and Paste when they'd do nothing. (Asking about
     * CF_UNICODETEXT covers CF_TEXT too, which Windows converts.) */
    MENU_PRIMARY_ITEM(IDM_COPY, L"&Copy", ICON_COPY,
                      wgs->term && wgs->term->selstate == SELECTED);
    MENU_PRIMARY_ITEM(IDM_PASTE, L"&Paste", ICON_PASTE,
                      IsClipboardFormatAvailable(CF_UNICODETEXT));
    MENU_SEP();

    /* The pools of text to paste again (see show_pool) */
    {
        bool sel = wgs->term && wgs->term->selstate == SELECTED &&
            conf_get_int(wgs->conf, CONF_copy_pool_size) > 0;
        menu_add(WUI_MENU_SUBMENU, 0, L"Cop&y to Pool", sel, false);
        menu_set_icon(ICON_POOL);
        menu_add(WUI_MENU_ITEM, IDM_POOL_SESSION, L"For this &session",
                 sel && WUI(wgs)->session_name, false);
        menu_add(WUI_MENU_ITEM, IDM_POOL_COMMON, L"For &all sessions",
                 sel, false);
        menu_add(WUI_MENU_END, 0, NULL, false, false);
        MENU_ICON_ITEM(IDM_POOL_PASTE, L"Paste from P&ool...", ICON_PASTE);
        menu_set_own_accel(L"Ctrl+Alt+V");
        MENU_SEP();
    }

    if (wgs->specials) {
        int nesting = 1;
        menu_add(WUI_MENU_SUBMENU, 0, L"S&pecial Command", true, false);
        menu_set_icon(ICON_COMMAND);
        for (int i = 0; nesting > 0; i++) {
            assert(IDM_SPECIAL_MIN + 0x10 * i < IDM_SPECIAL_MAX);
            switch (wgs->specials[i].code) {
              case SS_SEP:
                MENU_SEP();
                break;
              case SS_SUBMENU:
                nesting++;
                menu_add(WUI_MENU_SUBMENU, 0, menu_string(
                             dup_mb_to_wc(DEFAULT_CODEPAGE,
                                          wgs->specials[i].name)),
                         true, false);
                break;
              case SS_EXITMENU:
                nesting--;
                menu_add(WUI_MENU_END, 0, NULL, false, false);
                break;
              default:
                MENU_ITEM(IDM_SPECIAL_MIN + 0x10 * i, menu_string(
                              dup_mb_to_wc(DEFAULT_CODEPAGE,
                                           wgs->specials[i].name)));
                break;
            }
        }
        MENU_SEP();
    }

    MENU_ICON_ITEM(IDM_FIND, L"&Find...", ICON_FIND);
    MENU_SEP();
    {
        bool sftp = wgs->backend && sftp_xfer_available(wgs->backend);
        menu_add(WUI_MENU_ITEM, IDM_SENDFILE, L"Send Fil&e...", sftp, false);
        menu_set_icon(ICON_UPLOAD);
        menu_add(WUI_MENU_ITEM, IDM_RECVFILE, L"Recei&ve File...", sftp,
                 false);
        menu_set_icon(ICON_DOWNLOAD);
    }
    MENU_SEP();
    build_split_items(wgs);
    MENU_SEP();
    MENU_ICON_ITEM(IDM_COPYALL, L"C&opy All to Clipboard", ICON_SELECTALL);
    MENU_ICON_ITEM(IDM_CLRSB, L"C&lear Scrollback", ICON_DELETE);
    MENU_ICON_ITEM(IDM_RESET, L"Rese&t Terminal", ICON_REFRESH);
}

/* Splitting the tab, and closing a pane of it */
static void build_split_items(WinGuiSeat *wgs)
{
    MENU_ICON_ITEM(IDM_SPLITRIGHT, L"Split Ri&ght", ICON_SPLITRIGHT);
    menu_set_own_accel(L"Alt+Shift++");
    MENU_ICON_ITEM(IDM_SPLITDOWN, L"Split Do&wn", ICON_SPLITDOWN);
    menu_set_own_accel(L"Alt+Shift+-");
    if (WUI(wgs)->tterm && wui_term_is_split(WUI(wgs)->tterm)) {
        MENU_ICON_ITEM(IDM_CLOSEPANE, L"Close &Pane", ICON_CLOSEPANE);
        menu_set_own_accel(L"Ctrl+Shift+W");
    }
}

/* PuTTY's items in the window icon's menu (which is what right-clicking
 * the title bar, or Alt+Space, brings up too), after the system's */
const WuiMenuItem *wui_cb_get_window_menu(void *ctx, size_t *nitems)
{
    WinGuiSeat *wgs = &((WuiSeat *)ctx)->wgs;
    for (size_t i = 0; i < n_menu_strings; i++)
        sfree(menu_strings[i]);
    n_menu_strings = 0;
    menu_len = 0;
    menu_add(WUI_MENU_ITEM, IDM_FULLSCREEN, L"&Full Screen",
             conf_get_int(wgs->conf, CONF_resize_action) != RESIZE_DISABLED,
             is_full_screen(wgs));
    menu_set_icon(ICON_FULLSCREEN);
    /* (if that's turned on: Window > Behaviour) */
    if (conf_get_bool(wgs->conf, CONF_fullscreenonaltenter))
        menu_set_accel(L"Alt+Enter");
    MENU_SEP();
    MENU_ICON_ITEM(IDM_HELP, L"&Help", ICON_HELP);
    {
        char *str = dupprintf("&About %s", appname);
        MENU_ICON_ITEM(IDM_ABOUT, menu_string(
                           dup_mb_to_wc(DEFAULT_CODEPAGE, str)),
                       ICON_INFO);
        sfree(str);
    }
    *nitems = menu_len;
    return menu;
}

const WuiMenuItem *wui_cb_get_terminal_menu(void *ctx, size_t *nitems)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    build_menu(&ws->wgs);
    /* (in the welcome tab, there's nothing to do but copy its text, or
     * look for some) */
    if (ws->welcome)
        for (size_t i = 0; i < menu_len; i++)
            if (menu[i].id != IDM_COPY && menu[i].id != IDM_FIND)
                menu[i].enabled = false;
    *nitems = menu_len;
    return menu;
}

/* The menu of the saved sessions button, beside the caption buttons */
/* The saved sessions, for the button in the title bar */
static WuiSession *title_sessions;
static size_t n_title_sessions;

const WuiSession *wui_cb_get_sessions(size_t *nsessions)
{
    for (size_t i = 0; i < n_title_sessions; i++) {
        sfree((wchar_t *)title_sessions[i].name);
        sfree((wchar_t *)title_sessions[i].detail);
    }
    sfree(title_sessions);
    n_title_sessions = 0;

    /* (the commands are by where they are in sesslist) */
    get_sesslist(&sesslist, false);
    get_sesslist(&sesslist, true);
    size_t n;
    SessionInfo *si = sessions_get(&n);
    title_sessions = snewn(n ? n : 1, WuiSession);
    for (size_t i = 0; i < n; i++) {
        int j;
        for (j = 1; j < sesslist.nsessions && j <= MENU_SAVED_MAX; j++)
            if (!strcmp(sesslist.sessions[j], si[i].name))
                break;
        if (j >= sesslist.nsessions || j > MENU_SAVED_MAX)
            continue;
        WuiSession *ts = &title_sessions[n_title_sessions++];
        ts->name = dup_mb_to_wc(DEFAULT_CODEPAGE, si[i].name);
        ts->detail = session_subtitle(&si[i]);
        ts->favorite = si[i].favorite;
        ts->last_used = (long long)si[i].last_used;
        ts->id = IDM_SAVED_MIN + (j - 1) * MENU_SAVED_STEP;
    }
    sessions_free(si, n);
    *nsessions = n_title_sessions;
    return title_sessions;
}

bool wui_cb_session_folder_open(const wchar_t *folder)
{
    char *f = dup_wc_to_mb(DEFAULT_CODEPAGE, folder, "?");
    size_t n;
    char **closed = home_collapsed_get(&n);
    bool open = true;
    for (size_t i = 0; i < n && open; i++)
        open = strcmp(closed[i], f) != 0;
    folders_free(closed, n);
    sfree(f);
    return open;
}

unsigned wui_cb_sessions_hidden(void)
{
    return home_hidden_get();
}

void wui_cb_sessions_set_hidden(unsigned hidden)
{
    home_hidden_set(hidden);
}

unsigned wui_cb_sessions_omitted(void)
{
    return popup_omitted_get();
}

void wui_cb_sessions_set_omitted(unsigned omitted)
{
    popup_omitted_set(omitted);
}

void wui_cb_session_folder_toggle(const wchar_t *folder)
{
    char *f = dup_wc_to_mb(DEFAULT_CODEPAGE, folder, "?");
    size_t n;
    char **closed = home_collapsed_get(&n);
    size_t size = n, i;
    for (i = 0; i < n; i++)
        if (!strcmp(closed[i], f))
            break;
    if (i < n) {
        sfree(closed[i]);
        closed[i] = closed[--n];
        sfree(f);
    } else {
        sgrowarray(closed, size, n);
        closed[n++] = f;
    }
    home_collapsed_set(closed, n);
    folders_free(closed, n);
}

/* The menu you get by right-clicking a tab */
const WuiMenuItem *wui_cb_get_tab_menu(void *ctx, size_t *nitems)
{
    WuiSeat *ws = (WuiSeat *)ctx;

    for (size_t i = 0; i < n_menu_strings; i++)
        sfree(menu_strings[i]);
    n_menu_strings = 0;
    menu_len = 0;
    menu_own_accels = !conf_get_bool(ws->wgs.conf, CONF_no_wui_shortcuts);

    MENU_ICON_ITEM(IDM_NEWSESS, L"&New Tab", ICON_ADD);
    MENU_SEP();
    /* (then what's to do with this tab itself, and then its session) */
    MENU_ICON_ITEM(IDM_RENAMETAB, L"Re&name Tab", ICON_RENAME);
    menu_add(WUI_MENU_ITEM, IDM_TABLEFT, L"Move &Left",
             wui_term_can_move(ws->tterm, -1), false);
    menu_set_icon(ICON_BACK);
    menu_add(WUI_MENU_ITEM, IDM_TABRIGHT, L"Move &Right",
             wui_term_can_move(ws->tterm, +1), false);
    menu_set_icon(ICON_FORWARD);
    MENU_SEP();
    MENU_ICON_ITEM(IDM_DUPSESS, L"&Duplicate Session", ICON_COPY);
    /* (splitting is in the terminal's menu; closing a pane of a split
     * one is here too) */
    if (ws->tterm && wui_term_is_split(ws->tterm)) {
        MENU_ICON_ITEM(IDM_CLOSEPANE, L"Close &Pane", ICON_CLOSEPANE);
        menu_set_own_accel(L"Ctrl+Shift+W");
    }
    /* (while it's still connected too, after asking: see IDM_RESTART) */
    menu_add(WUI_MENU_ITEM, IDM_RESTART, L"R&estart Session", true, false);
    menu_set_icon(ICON_REFRESH);
    MENU_SEP();
    MENU_ICON_ITEM(IDM_RECONF, L"Chan&ge Settings...", ICON_SETTINGS);
    menu_set_own_accel(L"Ctrl+,");
    MENU_ICON_ITEM(IDM_SHOWLOG, L"&Event Log", ICON_HISTORY);
    MENU_SEP();
    MENU_ICON_ITEM(IDM_CLOSETAB, L"&Close Tab", ICON_CLOSE);
    menu_set_accel(L"Ctrl+F4");

    /* (in the welcome tab, there's nothing to do but start a session) */
    if (ws->welcome)
        for (size_t i = 0; i < menu_len; i++)
            if (menu[i].id != IDM_NEWSESS)
                menu[i].enabled = false;
    *nitems = menu_len;
    return menu;
}

void wui_cb_menu_command(void *ctx, unsigned id)
{
    /* The menu might have outlived its session, so check it's still
     * there before looking inside it */
    WuiSeat *ws;
    unsigned seat_id = 0;
    FOR_EACH_SEAT(ws) {
        if (ws == ctx)
            seat_id = ws->id;
    }

    /* Commands can open modal dialog boxes, which we'd rather not do
     * from inside a XAML event handler, so defer them to our own
     * message window. */
    PostMessage(msg_hwnd, WM_WUI_COMMAND, id, seat_id);
}

/*
 * Update the Special Commands submenu.
 */
static void win_seat_update_specials_menu(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    int i;

    if (wgs->backend)
        wgs->specials = backend_get_specials(wgs->backend);
    else
        wgs->specials = NULL;

    if (wgs->specials) {
        int nesting = 1;
        for (i = 0; nesting > 0; i++) {
            if (wgs->specials[i].code == SS_SUBMENU)
                nesting++;
            else if (wgs->specials[i].code == SS_EXITMENU)
                nesting--;
        }
        /* Squirrel the highest special. */
        wgs->n_specials = i - 1;
    } else {
        wgs->n_specials = 0;
    }
    /* The menus themselves are rebuilt from wgs->specials each time
     * they're shown. */
}

static void update_mouse_pointer(WinGuiSeat *wgs)
{
    WuiCursor curstype = WUI_CURSOR_IBEAM;
    bool force_visible = false;
    static bool forced_visible = false;
    switch (wgs->busy_status) {
      case BUSY_NOT:
        if (wgs->pointer_indicates_raw_mouse)
            curstype = WUI_CURSOR_ARROW;
        else
            curstype = WUI_CURSOR_IBEAM;
        break;
      case BUSY_WAITING:
        curstype = WUI_CURSOR_APPSTARTING; /* this may be an abuse */
        force_visible = true;
        break;
      case BUSY_CPU:
        curstype = WUI_CURSOR_WAIT;
        force_visible = true;
        break;
      default:
        unreachable("Bad busy_status");
    }
    if (WUI(wgs)->tterm)
        wui_set_cursor(WUI(wgs)->tterm, curstype);
    if (WUI(wgs) != active)
        return;   /* ShowCursor is global, so only for the visible tab */
    if (force_visible != forced_visible) {
        /* We want some cursor shapes to be visible always.
         * Along with show_mouseptr(), this manages the ShowCursor()
         * counter such that if we switch back to a non-force_visible
         * cursor, the previous visibility state is restored. */
        ShowCursor(force_visible);
        forced_visible = force_visible;
    }
}

static void win_seat_set_busy_status(Seat *seat, BusyStatus status)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    wgs->busy_status = status;
    update_mouse_pointer(wgs);
}

static void wintw_set_raw_mouse_mode(TermWin *tw, bool activate)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wgs->send_raw_mouse = activate;
}

static void wintw_set_raw_mouse_mode_pointer(TermWin *tw, bool activate)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wgs->pointer_indicates_raw_mouse = activate;
    update_mouse_pointer(wgs);
}

/*
 * A message over the window, in the WinUI style (or a message box, if
 * there's no window yet). With a 'primary' button, returns whether
 * that was pressed.
 */
static bool show_message(const char *title, const char *msg,
                         const wchar_t *primary, bool warning)
{
    if (!host) {
        UINT style = (primary ? MB_OKCANCEL : MB_OK) |
            (warning ? MB_ICONWARNING : MB_ICONINFORMATION);
        return MessageBox(NULL, msg, title, style) == IDOK;
    }
    wchar_t *wtitle = dup_mb_to_wc(DEFAULT_CODEPAGE, title);
    wchar_t *wmsg = dup_mb_to_wc(DEFAULT_CODEPAGE, msg);
    bool ret = wui_message(host, wtitle, wmsg, primary,
                           primary ? L"Cancel" : L"OK", warning);
    sfree(wtitle);
    sfree(wmsg);
    return ret;
}

/*
 * A security alert (the host key, or a weak algorithm) over the
 * window, in the WinUI style, from the Seat's dialog text: see
 * wui_alert. Returns -1 if it couldn't be shown, for the caller to use
 * PuTTY's own dialog box instead.
 */
static int show_alert(SeatDialogText *text, const wchar_t *primary,
                      const wchar_t *secondary, const wchar_t *close)
{
    if (!host)
        return -1;

    WuiAlert alert = { .primary = primary, .secondary = secondary,
                       .close = close };
    WuiAlertItem *items = snewn(text->nitems, WuiAlertItem);
    wchar_t *title = NULL, *heading = NULL;
    size_t n = 0;
    for (size_t i = 0; i < text->nitems; i++) {
        SeatDialogTextItem *item = &text->items[i];
        wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE, item->text);
        switch (item->type) {
          case SDT_TITLE:
            sfree(title);
            title = w;
            continue;
          case SDT_SCARY_HEADING:
            sfree(heading);
            heading = w;
            continue;
          case SDT_PARA:
            items[n].type = WUI_ALERT_PARA;
            break;
          case SDT_DISPLAY:
            items[n].type = WUI_ALERT_DISPLAY;
            break;
          case SDT_MORE_INFO_KEY:
            items[n].type = WUI_ALERT_INFO_KEY;
            break;
          case SDT_MORE_INFO_VALUE_SHORT:
            items[n].type = WUI_ALERT_INFO_VALUE;
            break;
          case SDT_MORE_INFO_VALUE_BLOB:
            items[n].type = WUI_ALERT_INFO_BLOB;
            break;
          default:
            /* (the console's prompts, not for us) */
            sfree(w);
            continue;
        }
        items[n++].text = w;
    }
    alert.title = title;
    alert.heading = heading;
    alert.items = items;
    alert.nitems = n;

    int ret = wui_alert(host, &alert);

    for (size_t i = 0; i < n; i++)
        sfree((wchar_t *)items[i].text);
    sfree(items);
    sfree(title);
    sfree(heading);
    return ret;
}

static SeatPromptResult wui_seat_confirm_ssh_host_key(
    Seat *seat, const char *host_name, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    int ret = show_alert(text, L"Accept", L"Connect Once", L"Cancel");
    if (ret < 0)
        return win_seat_confirm_ssh_host_key(
            seat, host_name, port, keytype, keystr, text, helpctx,
            callback, ctx);
    if (ret == 1) {
        store_host_key(seat, host_name, port, keytype, keystr);
        return SPR_OK;
    } else if (ret == 2) {
        return SPR_OK;
    }
    return SPR_USER_ABORT;
}

static SeatPromptResult wui_seat_confirm_weak_crypto_primitive(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    int ret = show_alert(text, L"Yes", NULL, L"No");
    if (ret < 0)
        return win_seat_confirm_weak_crypto_primitive(seat, text,
                                                      callback, ctx);
    return ret == 1 ? SPR_OK : SPR_USER_ABORT;
}

static SeatPromptResult wui_seat_confirm_weak_cached_hostkey(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    int ret = show_alert(text, L"Yes", NULL, L"No");
    if (ret < 0)
        return win_seat_confirm_weak_cached_hostkey(seat, text,
                                                    callback, ctx);
    return ret == 1 ? SPR_OK : SPR_USER_ABORT;
}

/*
 * The session has ended, for this reason: say so in its tab (with an
 * icon, and the reason in the tooltip) and in the terminal, rather
 * than in a message box that has to be dismissed.
 */
static void show_disconnected(WinGuiSeat *wgs, const char *msg)
{
    WuiSeat *ws = WUI(wgs);
    if (ws->tterm) {
        wchar_t *wmsg = dup_mb_to_wc(DEFAULT_CODEPAGE, msg);
        wui_term_set_status(ws->tterm, wmsg);
        sfree(wmsg);
    }
    if (wgs->term) {
        char *text = dupprintf("\r\n%s\r\n", msg);
        term_data(wgs->term, text, strlen(text));
        sfree(text);
    }
}

/*
 * Report a fatal error and close the connection.
 */
static void win_seat_connection_fatal(Seat *seat, const char *msg)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    show_disconnected(wgs, msg);

    if (conf_get_int(wgs->conf, CONF_close_on_exit) == FORCE_ON) {
        if (nseats == 1)
            exit_code = 1;
        queue_close_tab(wgs);
    } else {
        queue_toplevel_callback(close_session, wgs);
    }
}

/*
 * Print a message box and don't close the connection.
 */
static void win_seat_nonfatal(Seat *seat, const char *msg)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    char *title = dupprintf("%s Error", appname);
    show_mouseptr(wgs, true);
    show_message(title, msg, NULL, true);
    sfree(title);
}

static HWND find_window_for_msgbox(void)
{
    return host ? wui_hwnd(host) : NULL;
}

/*
 * Report an error at the command-line parsing stage.
 */
void cmdline_error(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    title = dupprintf("%s Command Line Error", appname);
    MessageBox(find_window_for_msgbox(), message, title, MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    exit(1);
}

static inline rgb rgb_from_colorref(COLORREF cr)
{
    rgb toret;
    toret.r = GetRValue(cr);
    toret.g = GetGValue(cr);
    toret.b = GetBValue(cr);
    return toret;
}

static void wintw_palette_get_overrides(TermWin *tw, Terminal *term)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (conf_get_bool(wgs->conf, CONF_system_colour)) {
        rgb rgb;

        rgb = rgb_from_colorref(GetSysColor(COLOR_WINDOWTEXT));
        term_palette_override(term, OSC4_COLOUR_fg, rgb);
        term_palette_override(term, OSC4_COLOUR_fg_bold, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_WINDOW));
        term_palette_override(term, OSC4_COLOUR_bg, rgb);
        term_palette_override(term, OSC4_COLOUR_bg_bold, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHTTEXT));
        term_palette_override(term, OSC4_COLOUR_cursor_fg, rgb);

        rgb = rgb_from_colorref(GetSysColor(COLOR_HIGHLIGHT));
        term_palette_override(term, OSC4_COLOUR_cursor_bg, rgb);
    }
}

/* ----------------------------------------------------------------------
 * Fonts. The actual font handling is done by DirectWrite on the C++
 * side; here we just decide what to ask for.
 */

static int codepage_from_charset(int charset)
{
    CHARSETINFO info;

    /* !!! Yes the next line is right */
    if (charset == OEM_CHARSET)
        return GetOEMCP();
    if (charset == DEFAULT_CHARSET || charset == ANSI_CHARSET)
        return GetACP();
    memset(&info, 0xFF, sizeof(info));
    if (TranslateCharsetInfo((DWORD *)(ULONG_PTR)charset,
                             &info, TCI_SRCCHARSET))
        return info.ciACP;
    return -1;
}

static void init_fonts(WinGuiSeat *wgs, int pick_width, int pick_height)
{
    FontSpec *font;
    WuiFontRequest req;
    WuiFontMetrics m;
    CPINFO cpinfo;

    wgs->bold_font_mode =
        conf_get_int(wgs->conf, CONF_bold_style) & BOLD_STYLE_FONT ?
        BOLD_FONT : BOLD_NONE;
    wgs->bold_colours =
        conf_get_int(wgs->conf, CONF_bold_style) & BOLD_STYLE_COLOUR ?
        true : false;
    /* We draw underlines by hand, which always fit in the cell */
    wgs->und_mode = UND_LINE;

    font = conf_get_fontspec(wgs->conf, CONF_font);

    memset(&req, 0, sizeof(req));
    wchar_t *wname = dup_mb_to_wc(DEFAULT_CODEPAGE, font->name);
    req.name = wname;
    req.bold = font->isbold;
    req.charset = font->charset;
    switch (conf_get_int(wgs->conf, CONF_font_quality)) {
      case FQ_ANTIALIASED: req.quality = WUI_QUALITY_ANTIALIASED; break;
      case FQ_NONANTIALIASED: req.quality = WUI_QUALITY_NONANTIALIASED; break;
      case FQ_CLEARTYPE: req.quality = WUI_QUALITY_CLEARTYPE; break;
      default: req.quality = WUI_QUALITY_DEFAULT; break;
    }
    if (pick_height) {
        req.cell_height = pick_height;
        req.cell_width = pick_width;
    } else if (font->height > 0) {
        req.em_height = MulDiv(font->height, wgs->dpi_info.cur_dpi.y, 72);
    } else if (font->height < 0) {
        req.em_height = -font->height;
    } else {
        req.em_height = MulDiv(10, wgs->dpi_info.cur_dpi.y, 72);
    }

    if (!wui_set_font(WUI(wgs)->tterm, &req, &m))
        modalfatalbox("Unable to load font \"%s\"", font->name);
    sfree(wname);

    wgs->font_width = m.cell_width;
    wgs->font_height = m.cell_height;
    wgs->font_strikethrough_y = m.strikethrough_y;
    wgs->font_varpitch = !m.monospaced;
    wgs->font_dualwidth = !m.monospaced;

    wgs->descent = m.ascent + 1;
    if (wgs->descent >= wgs->font_height)
        wgs->descent = wgs->font_height - 1;

    /* For the IME composition window */
    memset(&wgs->lfont, 0, sizeof(wgs->lfont));
    wgs->lfont.lfHeight = -(req.em_height ? req.em_height : m.cell_height);
    wgs->lfont.lfWeight = font->isbold ? FW_BOLD : FW_DONTCARE;
    wgs->lfont.lfCharSet = font->charset;
    strncpy(wgs->lfont.lfFaceName, font->name, LF_FACESIZE - 1);

    wgs->ucsdata.font_codepage = codepage_from_charset(font->charset);
    if (wgs->ucsdata.font_codepage <= 0 ||
        !GetCPInfo(wgs->ucsdata.font_codepage, &cpinfo))
        wgs->ucsdata.dbcs_screenfont = false;
    else
        wgs->ucsdata.dbcs_screenfont = (cpinfo.MaxCharSize > 1);

    if (WUI(wgs)->trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(WUI(wgs)->trust_icon);
    }
    WUI(wgs)->trust_icon = LoadImage(hinst, MAKEINTRESOURCE(IDI_MAINICON),
                                     IMAGE_ICON, wgs->font_width*2,
                                     wgs->font_height, LR_DEFAULTCOLOR);

    init_ucs(wgs->conf, &wgs->ucsdata);
}

static void deinit_fonts(WinGuiSeat *wgs)
{
    if (WUI(wgs)->trust_icon != INVALID_HANDLE_VALUE) {
        DestroyIcon(WUI(wgs)->trust_icon);
    }
    WUI(wgs)->trust_icon = INVALID_HANDLE_VALUE;
}

/* ----------------------------------------------------------------------
 * Window layout.
 *
 * window.c works in terms of the window rectangle and the client
 * rectangle. Here the role of the client rectangle is played by the
 * session's terminal drawing area, so the scroll bar and the tab
 * strip count as part of the window furniture, just as WS_VSCROLL's
 * scroll bar does.
 */

static void get_rects(WinGuiSeat *wgs, RECT *wr, RECT *cr)
{
    int pw, ph;
    GetWindowRect(wgs->term_hwnd, wr);
    wui_get_panel_size(WUI(wgs)->tterm, &pw, &ph);
    cr->left = cr->top = 0;
    cr->right = pw;
    cr->bottom = ph;
}

/* Resize the whole window to the given size, by resizing the
 * terminal area to whatever that leaves for it. */
static void set_outer_size(WinGuiSeat *wgs, int width, int height)
{
    RECT wr, cr;
    get_rects(wgs, &wr, &cr);
    int nonpanel_w = (wr.right - wr.left) - cr.right;
    int nonpanel_h = (wr.bottom - wr.top) - cr.bottom;
    wui_set_panel_size(WUI(wgs)->tterm,
                       width - nonpanel_w, height - nonpanel_h);
}

static void invalidate_all(WinGuiSeat *wgs)
{
    WUI(wgs)->border_dirty = true;
    if (wgs->term)
        term_invalidate(wgs->term);
}

static void wintw_request_resize(TermWin *tw, int w, int h)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    const struct BackendVtable *vt;
    int width, height;
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    bool deny_resize = false;

    /* Suppress server-originated resizing attempts if local resizing
     * is disabled entirely, or if it's supposed to change
     * rows/columns but the window size isn't ours to change. */
    if (resize_action == RESIZE_DISABLED
        || (resize_action == RESIZE_TERM && is_zoomed(wgs))) {
        deny_resize = true;
    }

    vt = backend_vt_from_proto(be_default_protocol);
    if (vt && vt->flags & BACKEND_RESIZE_FORBIDDEN)
        deny_resize = true;
    if (h == wgs->term->rows && w == wgs->term->cols) deny_resize = true;

    /* We still need to acknowledge a suppressed resize attempt. */
    if (deny_resize) {
        term_resize_request_completed(wgs->term);
        return;
    }

    /* Sanity checks ... */
    {
        RECT ss;
        if (get_fullscreen_rect(wgs, &ss)) {
            /* Make sure the values aren't too big */
            width = (ss.right - ss.left - wgs->extra_width) / 4;
            height = (ss.bottom - ss.top - wgs->extra_height) / 6;

            if (w > width || h > height) {
                term_resize_request_completed(wgs->term);
                return;
            }
            if (w < 15)
                w = 15;
            if (h < 1)
                h = 1;
        }
    }

    if (resize_action != RESIZE_FONT && !is_zoomed(wgs)) {
        width = wgs->extra_width + wgs->font_width * w;
        height = wgs->extra_height + wgs->font_height * h;

        set_outer_size(wgs, width, height);
    } else {
        /*
         * If we're resizing by changing the font, we must tell the
         * terminal the new size immediately, so that reset_window
         * will know what to do.
         */
        term_size(wgs->term, h, w, conf_get_int(wgs->conf, CONF_savelines));
        reset_window(wgs, 0);
    }

    term_resize_request_completed(wgs->term);
    invalidate_all(wgs);
}

static void recompute_window_offset(WinGuiSeat *wgs)
{
    RECT wr, cr;
    get_rects(wgs, &wr, &cr);

    int win_width  = cr.right - cr.left;
    int win_height = cr.bottom - cr.top;

    int new_offset_width = (win_width-wgs->font_width*wgs->term->cols)/2;
    int new_offset_height = (win_height-wgs->font_height*wgs->term->rows)/2;

    if (wgs->offset_width != new_offset_width ||
        wgs->offset_height != new_offset_height) {
        wgs->offset_width = new_offset_width;
        wgs->offset_height = new_offset_height;
        invalidate_all(wgs);
    }
}

static void reset_window(WinGuiSeat *wgs, int reinit)
{
    /*
     * This function decides how to resize or redraw when the
     * user changes something.
     *
     * This function doesn't like to change the terminal size but if the
     * font size is locked that may be it's only soluion.
     */
    int win_width, win_height, resize_action, window_border;
    RECT cr, wr;

    if (!wgs->term)
        return;

    /* Current window sizes ... */
    get_rects(wgs, &wr, &cr);

    win_width  = cr.right - cr.left;
    win_height = cr.bottom - cr.top;

    resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    window_border = conf_get_int(wgs->conf, CONF_window_border);

    if (resize_action == RESIZE_DISABLED)
        reinit = 2;

    /* Are we being forced to reload the fonts ? */
    if (reinit>1) {
        deinit_fonts(wgs);
        init_fonts(wgs, 0, 0);
    }

    /* Oh, looks like we're minimised */
    if (win_width == 0 || win_height == 0 ||
        win_state == WUI_STATE_MINIMISED)
        return;

    /* Is the window out of position ? */
    if (!reinit) {
        recompute_window_offset(wgs);
    }

    if (is_zoomed(wgs)) {
        /* We're fullscreen (or sharing the window with other tabs),
         * which means we must not change the size of the window, so
         * it's the font size or the terminal itself.
         */

        wgs->extra_width = wr.right - wr.left - cr.right + cr.left;
        wgs->extra_height = wr.bottom - wr.top - cr.bottom + cr.top;

        if (resize_action != RESIZE_TERM) {
            if (wgs->font_width != win_width/wgs->term->cols ||
                wgs->font_height != win_height/wgs->term->rows) {
                int fw = (win_width - 2*window_border) / wgs->term->cols;
                int fh = (win_height - 2*window_border) / wgs->term->rows;
                /* In case that subtraction made the font size go
                 * negative in an edge case, bound it below by 1 */
                if (fw < 1) fw = 1;
                if (fh < 1) fh = 1;
                deinit_fonts(wgs);
                init_fonts(wgs, fw, fh);
                wgs->offset_width =
                    (win_width - wgs->font_width*wgs->term->cols) / 2;
                wgs->offset_height =
                    (win_height - wgs->font_height*wgs->term->rows) / 2;
                invalidate_all(wgs);
            }
        } else {
            if (wgs->font_width * wgs->term->cols != win_width ||
                wgs->font_height * wgs->term->rows != win_height) {
                /* Our only choice at this point is to change the
                 * size of the terminal; Oh well.
                 */
                term_size(wgs->term,
                          (win_height - 2*window_border) / wgs->font_height,
                          (win_width - 2*window_border) / wgs->font_width,
                          conf_get_int(wgs->conf, CONF_savelines));
                wgs->offset_width =
                    (win_width - window_border - wgs->font_width*wgs->term->cols) / 2;
                wgs->offset_height =
                    (win_height - window_border - wgs->font_height*wgs->term->rows) / 2;
                invalidate_all(wgs);
            }
        }
        return;
    }

    /* Resize the terminal area to suit the new fonts after a DPI
     * change. (The window itself has already been moved and resized
     * to suit the new monitor by the framework.) */
    if (reinit == 3) {
        wgs->offset_width = wgs->offset_height = window_border;
        wui_set_panel_size(WUI(wgs)->tterm,
                           wgs->font_width * wgs->term->cols +
                           2 * window_border,
                           wgs->font_height * wgs->term->rows +
                           2 * window_border);
        get_rects(wgs, &wr, &cr);
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;
        invalidate_all(wgs);
        return;
    }

    /* Hmm, a force re-init means we should ignore the current window
     * so we resize to the default font size.
     */
    if (reinit>0) {
        wgs->offset_width = wgs->offset_height = window_border;
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        if (win_width != (wgs->font_width*wgs->term->cols +
                          wgs->offset_width*2) ||
            win_height != (wgs->font_height*wgs->term->rows +
                           wgs->offset_height*2)) {

            /* If this is too large windows will resize it to the maximum
             * allowed window size, we will then be back in here and resize
             * the font or terminal to fit.
             */
            set_outer_size(
                wgs, wgs->font_width*wgs->term->cols + wgs->extra_width,
                wgs->font_height*wgs->term->rows + wgs->extra_height);
        }

        invalidate_all(wgs);
        return;
    }

    /* Okay the user doesn't want us to change the font so we try the
     * window. But that may be too big for the screen which forces us
     * to change the terminal.
     */
    if ((resize_action == RESIZE_TERM && reinit<=0) ||
        (resize_action == RESIZE_EITHER && reinit<0) ||
        reinit>0) {
        wgs->offset_width = wgs->offset_height = window_border;
        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        if (win_width != (wgs->font_width*wgs->term->cols +
                          wgs->offset_width*2) ||
            win_height != (wgs->font_height*wgs->term->rows +
                           wgs->offset_height*2)) {

            RECT ss;
            int width, height;

            get_fullscreen_rect(wgs, &ss);

            width = (ss.right - ss.left - wgs->extra_width) / wgs->font_width;
            height = (ss.bottom - ss.top - wgs->extra_height)/wgs->font_height;

            /* Grrr too big */
            if ( wgs->term->rows > height || wgs->term->cols > width ) {
                if (resize_action == RESIZE_EITHER) {
                    /* Make the font the biggest we can */
                    if (wgs->term->cols > width)
                        wgs->font_width =
                            (ss.right - ss.left - wgs->extra_width) /
                            wgs->term->cols;
                    if (wgs->term->rows > height)
                        wgs->font_height =
                            (ss.bottom - ss.top - wgs->extra_height) /
                            wgs->term->rows;

                    deinit_fonts(wgs);
                    init_fonts(wgs, wgs->font_width, wgs->font_height);

                    width = (ss.right - ss.left - wgs->extra_width) /
                        wgs->font_width;
                    height = (ss.bottom - ss.top - wgs->extra_height) /
                        wgs->font_height;
                } else {
                    if ( height > wgs->term->rows ) height = wgs->term->rows;
                    if ( width > wgs->term->cols )  width = wgs->term->cols;
                    term_size(wgs->term, height, width,
                              conf_get_int(wgs->conf, CONF_savelines));
                }
            }

            set_outer_size(
                wgs, wgs->font_width*wgs->term->cols + wgs->extra_width,
                wgs->font_height*wgs->term->rows + wgs->extra_height);

            invalidate_all(wgs);
        }
        return;
    }

    /* We're allowed to or must change the font but do we want to ?  */

    if (wgs->font_width != (win_width-window_border*2)/wgs->term->cols ||
        wgs->font_height != (win_height-window_border*2)/wgs->term->rows) {

        deinit_fonts(wgs);
        init_fonts(wgs, (win_width-window_border*2)/wgs->term->cols,
                   (win_height-window_border*2)/wgs->term->rows);
        wgs->offset_width = (win_width-wgs->font_width*wgs->term->cols)/2;
        wgs->offset_height = (win_height-wgs->font_height*wgs->term->rows)/2;

        wgs->extra_width =
            wr.right - wr.left - cr.right + cr.left + wgs->offset_width*2;
        wgs->extra_height =
            wr.bottom - wr.top - cr.bottom + cr.top + wgs->offset_height*2;

        invalidate_all(wgs);
    }
}

static void set_input_locale(WinGuiSeat *wgs, HKL kl)
{
    char lbuf[20];

    GetLocaleInfo(LOWORD(kl), LOCALE_IDEFAULTANSICODEPAGE,
                  lbuf, sizeof(lbuf));

    wgs->kbd_codepage = atoi(lbuf);
}

static void click(WinGuiSeat *wgs, Mouse_Button b, int x, int y,
                  bool shift, bool ctrl, bool alt, int thistime)
{
    if (wgs->send_raw_mouse &&
        !(shift && conf_get_bool(wgs->conf, CONF_mouse_override))) {
        wgs->lastbtn = MBT_NOTHING;
        term_mouse(wgs->term, b, translate_button(wgs, b), MA_CLICK,
                   x, y, shift, ctrl, alt);
        return;
    }

    if (wgs->lastbtn == b && thistime - wgs->lasttime < wgs->dbltime) {
        wgs->lastact = (wgs->lastact == MA_CLICK ? MA_2CLK :
                   wgs->lastact == MA_2CLK ? MA_3CLK :
                   wgs->lastact == MA_3CLK ? MA_CLICK : MA_NOTHING);
    } else {
        wgs->lastbtn = b;
        wgs->lastact = MA_CLICK;
    }
    if (wgs->lastact != MA_NOTHING)
        term_mouse(wgs->term, b, translate_button(wgs, b), wgs->lastact,
                   x, y, shift, ctrl, alt);
    wgs->lasttime = thistime;
}

/*
 * Translate a raw mouse button designation (LEFT, MIDDLE, RIGHT)
 * into a cooked one (SELECT, EXTEND, PASTE).
 */
static Mouse_Button translate_button(WinGuiSeat *wgs, Mouse_Button button)
{
    if (button == MBT_LEFT)
        return MBT_SELECT;
    if (button == MBT_MIDDLE)
        return conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_XTERM ?
            MBT_PASTE : MBT_EXTEND;
    if (button == MBT_RIGHT)
        return conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_XTERM ?
            MBT_EXTEND : MBT_PASTE;
    return 0;                          /* shouldn't happen */
}

static void show_mouseptr(WinGuiSeat *wgs, bool show)
{
    /* NB that the counter in ShowCursor() is also frobbed by
     * update_mouse_pointer() */
    static bool cursor_visible = true;
    if (wgs) {
        if (!conf_get_bool(wgs->conf, CONF_hide_mouseptr))
            show = true; /* hiding mouse pointer disabled in Conf */
    } else {
        /*
         * You can pass wgs==NULL if you want to _show_ the pointer
         * rather than hiding it, because that's never disallowed.
         */
        assert(show);
    }
    if (cursor_visible && !show)
        ShowCursor(false);
    else if (!cursor_visible && show)
        ShowCursor(true);
    cursor_visible = show;
}

static bool is_alt_pressed(void)
{
    BYTE keystate[256];
    int r = GetKeyboardState(keystate);
    if (!r)
        return false;
    if (keystate[VK_MENU] & 0x80)
        return true;
    if (keystate[VK_RMENU] & 0x80)
        return true;
    return false;
}

static void exit_callback(void *vctx)
{
    WinGuiSeat *wgs = (WinGuiSeat *)vctx;
    int exitcode, close_on_exit;

    if (!wgs->session_closed &&
        (exitcode = backend_exitcode(wgs->backend)) >= 0) {
        close_on_exit = conf_get_int(wgs->conf, CONF_close_on_exit);
        /* Abnormal exits will already have set session_closed and taken
         * appropriate action. */
        if (close_on_exit == FORCE_ON ||
            (close_on_exit == AUTO && exitcode != INT_MAX)) {
            queue_close_tab(wgs);
        } else {
            queue_toplevel_callback(close_session, wgs);
            wgs->session_closed = true;
            /* exitcode == INT_MAX indicates that the connection was closed
             * by a fatal error, so an error box will be coming our way and
             * we should not generate this informational one. */
            if (exitcode != INT_MAX)
                show_disconnected(wgs, "Connection closed by remote host");
        }
    }
}

static void win_seat_notify_remote_exit(Seat *seat)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    queue_toplevel_callback(exit_callback, wgs);
}

static void conf_cache_data(WinGuiSeat *wgs)
{
    /* Cache some items from conf to speed lookups in very hot code */
    wgs->cursor_type = conf_get_int(wgs->conf, CONF_cursor_type);
    wgs->vtmode = conf_get_int(wgs->conf, CONF_vtmode);
}

static const int clips_system[] = { CLIP_SYSTEM };

/* ----------------------------------------------------------------------
 * Window events from the XAML side.
 */

/* Close the tab a session's in: all its panes, asking first (once) if
 * any of them want that */
static void confirm_close_tab(WinGuiSeat *wgs)
{
    void *ctxs[256];
    size_t n = wui_term_tab_panes(WUI(wgs)->tterm, ctxs, lenof(ctxs));
    if (n > lenof(ctxs))
        n = lenof(ctxs);
    if (n <= 1) {
        confirm_close_session(wgs);
        return;
    }

    unsigned ids[lenof(ctxs)];
    int nwarn = 0;
    for (size_t i = 0; i < n; i++) {
        WuiSeat *ws = (WuiSeat *)ctxs[i];
        ids[i] = ws->id;
        if (!ws->welcome && !ws->wgs.session_closed &&
            conf_get_bool(ws->wgs.conf, CONF_warn_on_close))
            nwarn++;
    }
    show_mouseptr(wgs, true);
    frontend_busy_enter();
    bool go = true;
    if (nwarn) {
        char *msg = nwarn == 1 ?
            dupprintf("Are you sure you want to close this tab? One "
                      "session in it is still active.") :
            dupprintf("Are you sure you want to close this tab? %d "
                      "sessions in it are still active.", nwarn);
        go = show_message("Close Tab", msg, L"Close", true);
        sfree(msg);
    }
    if (go) {
        for (size_t i = 0; i < n; i++) {
            WuiSeat *ws = find_seat(ids[i]);
            if (ws)
                close_tab(&ws->wgs);
        }
    }
    frontend_busy_leave();
}

/* Close one session (and its pane, or its tab if it's the only one
 * there), asking first if it wants that */
static void confirm_close_session(WinGuiSeat *wgs)
{
    char *title, *msg, *additional = NULL;
    show_mouseptr(wgs, true);
    title = dupprintf("Close Session");
    if (wgs->backend && wgs->backend->vt->close_warn_text) {
        additional = wgs->backend->vt->close_warn_text(wgs->backend);
    }
    msg = dupprintf("Are you sure you want to close this session?%s%s",
                    additional ? "\n" : "",
                    additional ? additional : "");
    frontend_busy_enter();
    if (WUI(wgs)->welcome || wgs->session_closed ||
        !conf_get_bool(wgs->conf, CONF_warn_on_close) ||
        show_message(title, msg, L"Close", true))
        close_tab(wgs);
    frontend_busy_leave();
    sfree(title);
    sfree(msg);
    sfree(additional);
}

static void confirm_close_window(void)
{
    WuiSeat *ws, *warn = NULL;
    int nwarn = 0;

    /* Which sessions would the user want to be asked about? (Not the
     * welcome tab, which has none in it) */
    FOR_EACH_SEAT(ws) {
        if (!ws->welcome && !ws->wgs.session_closed &&
            conf_get_bool(ws->wgs.conf, CONF_warn_on_close)) {
            warn = ws;
            nwarn++;
        }
    }

    if (nwarn == 0) {
        wui_close_window(host);
    } else if (nwarn == 1 && nseats == 1) {
        /* Just the one: ask exactly as we would for closing its tab */
        WinGuiSeat *wgs = &warn->wgs;
        char *title, *msg, *additional = NULL;
        show_mouseptr(wgs, true);
        title = dupprintf("Close Session");
        if (wgs->backend && wgs->backend->vt->close_warn_text)
            additional = wgs->backend->vt->close_warn_text(wgs->backend);
        msg = dupprintf("Are you sure you want to close this session?%s%s",
                        additional ? "\n" : "",
                        additional ? additional : "");
        frontend_busy_enter();
        if (show_message(title, msg, L"Close", true))
            wui_close_window(host);
        frontend_busy_leave();
        sfree(title);
        sfree(msg);
        sfree(additional);
    } else {
        char *title = dupprintf("Close Window");
        char *msg = (nwarn == 1 ?
                     dupprintf("Are you sure you want to close this window? "
                               "One session is still active.") :
                     dupprintf("Are you sure you want to close this window? "
                               "%d sessions are still active.", nwarn));
        if (active)
            show_mouseptr(&active->wgs, true);
        frontend_busy_enter();
        if (show_message(title, msg, L"Close", true))
            wui_close_window(host);
        frontend_busy_leave();
        sfree(title);
        sfree(msg);
    }
}

void wui_cb_close_requested(void)
{
    PostMessage(msg_hwnd, WM_WUI_CLOSE, 0, 0);
}

void wui_cb_closed(void)
{
    show_mouseptr(NULL, true);
    wui_exit();
}

static void resize_term_to_panel(WinGuiSeat *wgs, int width, int height)
{
    int border_size = conf_get_int(wgs->conf, CONF_window_border);

    int w = (width - border_size*2) / wgs->font_width;
    int h = (height - border_size*2) / wgs->font_height;

    if (w < 1) w = 1;
    if (h < 1) h = 1;

    if (wgs->resizing) {
        /*
         * If we're in the middle of an interactive resize, we don't
         * call term_size. This means that, firstly, the user can drag
         * the size back and forth indecisively without wiping out any
         * actual terminal contents, and secondly, the Terminal
         * doesn't call back->size in turn for each increment of the
         * resizing drag, so we don't spam the server with huge
         * numbers of resize events.
         */
        wgs->need_backend_resize = true;
    } else {
        term_size(wgs->term, h, w,
                  conf_get_int(wgs->conf, CONF_savelines));
    }
    conf_set_int(wgs->conf, CONF_height, h);
    conf_set_int(wgs->conf, CONF_width, w);
}

/*
 * The equivalent of window.c's WM_SIZE handler, called whenever a
 * session's terminal area changes size or the window changes state.
 */
static void handle_size(WinGuiSeat *wgs, int width, int height)
{
    WuiWindowState state = win_state;
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);


    if (!wgs->term)
        return;

    term_notify_minimised(wgs->term, state == WUI_STATE_MINIMISED);
    {
        /*
         * Historic PuTTY practice is that we tell the terminal the
         * size of the overall window.
         */
        RECT r;
        GetWindowRect(wgs->term_hwnd, &r);
        term_notify_window_size_pixels(
            wgs->term, r.right - r.left, r.bottom - r.top);
    }

    if (resize_action == RESIZE_DISABLED) {
        /* A resize, well it better be a minimize. */
        reset_window(wgs, -1);
    } else {
        if (is_zoomed(wgs)) {
            /* (Including when other tabs share the window) */
            wgs->was_zoomed = true;
            wgs->prev_rows = wgs->term->rows;
            wgs->prev_cols = wgs->term->cols;
            if (resize_action == RESIZE_TERM)
                resize_term_to_panel(wgs, width, height);
            reset_window(wgs, 0);
        } else if (state == WUI_STATE_NORMAL && wgs->was_zoomed) {
            wgs->was_zoomed = false;
            if (wgs->resizing) {
                /*
                 * Unmaximised by dragging the window off the top of the
                 * screen (or out of a snap): it's still being dragged,
                 * and the terminal hasn't been resized yet (see
                 * resize_term_to_panel), so making the window fit the
                 * terminal now would make it as big as it was again,
                 * under the mouse. That waits for the end of the drag.
                 */
                if (resize_action == RESIZE_TERM)
                    resize_term_to_panel(wgs, width, height);
                WUI(wgs)->unzoom_pending = true;
            } else if (resize_action == RESIZE_TERM) {
                resize_term_to_panel(wgs, width, height);
                reset_window(wgs, 2);
            } else if (resize_action != RESIZE_FONT)
                reset_window(wgs, 2);
            else
                reset_window(wgs, 0);
        } else if (state == WUI_STATE_MINIMISED) {
            /* do nothing */
        } else if (resize_action == RESIZE_TERM ||
                   (resize_action == RESIZE_EITHER &&
                    !is_alt_pressed())) {
            resize_term_to_panel(wgs, width, height);

            /*
             * Sometimes, we can get a spontaneous resize event
             * outside an interactive drag which wants to set us to a
             * new specific size (e.g. Windows+Right then Windows+Up).
             * If we've responded here by resizing the terminal, we
             * may still need to recompute the border around the
             * window and do a full redraw to clear the new border.
             */
            if (!wgs->resizing)
                recompute_window_offset(wgs);
        } else {
            reset_window(wgs, 0);
        }
    }
    sys_cursor_update(wgs);
    WUI(wgs)->border_dirty = true;
}

void wui_cb_panel_resized(void *ctx, int w, int h)
{
    handle_size(&((WuiSeat *)ctx)->wgs, w, h);
}

void wui_cb_state_changed(WuiWindowState state)
{
    WuiSeat *ws;
    if (state == win_state)
        return;
    /*
     * Coming back from maximised or full screen, the panels haven't
     * been laid out at the restored size yet: their size now is still
     * the big one, and fitting the terminal to that (and then the
     * window to the terminal, as handle_size does on unzooming) would
     * keep the window big. So leave it to the layout to come, which
     * calls handle_size with the size it's restored to.
     */
    bool unzoomed = state == WUI_STATE_NORMAL &&
        (win_state == WUI_STATE_MAXIMISED ||
         win_state == WUI_STATE_FULLSCREEN);
    win_state = state;
    update_window_title();
    FOR_EACH_SEAT(ws) {
        int w, h;
        update_scrollbar_visibility(&ws->wgs);
        if (unzoomed)
            continue;
        wui_get_panel_size(ws->tterm, &w, &h);
        handle_size(&ws->wgs, w, h);
    }
}

void wui_cb_dpi_changed(int dpi)
{
    WuiSeat *ws;
    FOR_EACH_SEAT(ws) {
        WinGuiSeat *wgs = &ws->wgs;
        if (dpi == wgs->dpi_info.cur_dpi.x)
            continue;
        wgs->dpi_info.cur_dpi.x = wgs->dpi_info.cur_dpi.y = dpi;
        reset_window(wgs, 3);
    }
}

void wui_cb_redraw(void *ctx)
{
    invalidate_all(&((WuiSeat *)ctx)->wgs);
}

void wui_cb_focus(void *ctx, bool focused)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;

    if (focused == ws->terminal_focused)
        return;
    ws->terminal_focused = focused;

    if (!wgs->term)
        return;

    if (focused) {
        term_set_focus(wgs->term, true);
        CreateCaret(GetFocus(), wgs->caretbm,
                    wgs->font_width, wgs->font_height);
        ShowCaret(GetFocus());
        flash_window(wgs, 0);               /* stop */
        wgs->compose_state = 0;
        term_update(wgs->term);
    } else {
        show_mouseptr(wgs, true);
        term_set_focus(wgs->term, false);
        DestroyCaret();
        wgs->caret_x = wgs->caret_y = -1; /* ensure caret replaced next time */
        term_update(wgs->term);
    }
}

/*
 * The equivalent of window.c's WM_SIZING handler: keep the size tip
 * up to date, and make sure the window size is _stepped_ in units of
 * the font size.
 */
static bool sizing(WinGuiSeat *wgs, WPARAM wParam, RECT *r)
{
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);

    if (!wgs->term)
        return false;
    /* (With several panes in the tab, the window's size isn't a whole
     * number of any one terminal's characters) */
    if (WUI(wgs)->tterm && wui_term_is_split(WUI(wgs)->tterm))
        return false;

    if (resize_action == RESIZE_TERM ||
        (resize_action == RESIZE_EITHER && !is_alt_pressed())) {
        int width, height, w, h, ew, eh;

        if (!wgs->need_backend_resize && resize_action == RESIZE_EITHER &&
            (conf_get_int(wgs->conf, CONF_height) != wgs->term->rows ||
             conf_get_int(wgs->conf, CONF_width) != wgs->term->cols)) {
            /*
             * Great! It seems that both the terminal size and the
             * font size have been changed and the user is now dragging.
             *
             * It will now be difficult to get back to the configured
             * font size!
             *
             * This would be easier but it seems to be too confusing.
             */
            conf_set_int(wgs->conf, CONF_height, wgs->term->rows);
            conf_set_int(wgs->conf, CONF_width, wgs->term->cols);

            invalidate_all(wgs);
            wgs->need_backend_resize = true;
        }

        width = r->right - r->left - wgs->extra_width;
        height = r->bottom - r->top - wgs->extra_height;
        w = (width + wgs->font_width / 2) / wgs->font_width;
        if (w < 1)
            w = 1;
        h = (height + wgs->font_height / 2) / wgs->font_height;
        if (h < 1)
            h = 1;
        UpdateSizeTip(wgs->term_hwnd, w, h);
        ew = width - w * wgs->font_width;
        eh = height - h * wgs->font_height;
        if (ew != 0) {
            if (wParam == WMSZ_LEFT ||
                wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
                r->left += ew;
            else
                r->right -= ew;
        }
        if (eh != 0) {
            if (wParam == WMSZ_TOP ||
                wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
                r->top += eh;
            else
                r->bottom -= eh;
        }
        return ew || eh;
    } else {
        int width, height, w, h;
        bool rv = false;
        int window_border = conf_get_int(wgs->conf, CONF_window_border);
        int ex_width = wgs->extra_width +
            (window_border - wgs->offset_width) * 2;
        int ex_height = wgs->extra_height +
            (window_border - wgs->offset_height) * 2;

        width = r->right - r->left - ex_width;
        height = r->bottom - r->top - ex_height;

        w = (width + wgs->term->cols/2)/wgs->term->cols;
        h = (height + wgs->term->rows/2)/wgs->term->rows;
        if ( r->right != r->left + w*wgs->term->cols + ex_width)
            rv = true;

        if (wParam == WMSZ_LEFT ||
            wParam == WMSZ_BOTTOMLEFT || wParam == WMSZ_TOPLEFT)
            r->left = r->right - w*wgs->term->cols - ex_width;
        else
            r->right = r->left + w*wgs->term->cols + ex_width;

        if (r->bottom != r->top + h*wgs->term->rows + ex_height)
            rv = true;

        if (wParam == WMSZ_TOP ||
            wParam == WMSZ_TOPRIGHT || wParam == WMSZ_TOPLEFT)
            r->top = r->bottom - h*wgs->term->rows - ex_height;
        else
            r->bottom = r->top + h*wgs->term->rows + ex_height;

        return rv;
    }
}

static void scroll_cmd(WinGuiSeat *wgs, WuiScrollKind kind, int pos)
{
    switch (kind) {
      case WUI_SCROLL_BOTTOM:
        term_scroll(wgs->term, -1, 0);
        break;
      case WUI_SCROLL_TOP:
        term_scroll(wgs->term, +1, 0);
        break;
      case WUI_SCROLL_LINEDOWN:
        term_scroll(wgs->term, 0, +1);
        break;
      case WUI_SCROLL_LINEUP:
        term_scroll(wgs->term, 0, -1);
        break;
      case WUI_SCROLL_PAGEDOWN:
        term_scroll(wgs->term, 0, +wgs->term->rows / 2);
        break;
      case WUI_SCROLL_PAGEUP:
        term_scroll(wgs->term, 0, -wgs->term->rows / 2);
        break;
      case WUI_SCROLL_POSITION:
        term_scroll(wgs->term, 1, pos);
        break;
    }
}

void wui_cb_scroll(void *ctx, WuiScrollKind kind, int pos)
{
    WinGuiSeat *wgs = &((WuiSeat *)ctx)->wgs;
    if (wgs->term)
        scroll_cmd(wgs, kind, pos);
}

/* ----------------------------------------------------------------------
 * Menu commands.
 */

/* What IDM_DUPSESS copies into the new window, if not the session's
 * own settings */
static Conf *launch_conf;

static void launch_new_session(WinGuiSeat *wgs, unsigned id)
{
    char b[2048];
    char *cl;
    const char *argprefix;
    bool inherit_handles;
    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    HANDLE filemap = NULL;

    if (restricted_acl())
        argprefix = "&R";
    else
        argprefix = "";

    if (id == IDM_DUPSESS) {
        /*
         * Allocate a file-mapping memory chunk for the
         * config structure.
         */
        SECURITY_ATTRIBUTES sa;
        strbuf *serbuf;
        void *p;
        int size;

        serbuf = strbuf_new();
        conf_serialise(BinarySink_UPCAST(serbuf),
                       launch_conf ? launch_conf : wgs->conf);
        size = serbuf->len;

        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = true;
        filemap = CreateFileMapping(INVALID_HANDLE_VALUE,
                                    &sa,
                                    PAGE_READWRITE,
                                    0, size, NULL);
        if (filemap && filemap != INVALID_HANDLE_VALUE) {
            p = MapViewOfFile(filemap, FILE_MAP_WRITE, 0, 0, size);
            if (p) {
                memcpy(p, serbuf->s, size);
                UnmapViewOfFile(p);
            }
        }

        strbuf_free(serbuf);
        inherit_handles = true;
        cl = dupprintf("putty %s&%p:%u", argprefix,
                       filemap, (unsigned)size);
    } else if (id >= IDM_SAVED_MIN && id < IDM_SAVED_MAX) {
        unsigned int sessno = ((id - IDM_SAVED_MIN)
                               / MENU_SAVED_STEP) + 1;
        if (sessno < (unsigned)sesslist.nsessions) {
            const char *session = sesslist.sessions[sessno];
            session_touch(session);
            cl = dupprintf("putty %s@%s", argprefix, session);
            inherit_handles = false;
        } else
            return;
    } else /* IDM_NEWWIN */ {
        cl = dupprintf("putty%s%s",
                       *argprefix ? " " : "",
                       argprefix);
        inherit_handles = false;
    }

    GetModuleFileName(NULL, b, sizeof(b) - 1);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcess(b, cl, NULL, NULL, inherit_handles,
                      NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (filemap)
        CloseHandle(filemap);
    sfree(cl);
}

/*
 * Open a new session in a tab: a fresh one via the config box, a
 * copy of an existing one, or a saved session.
 */
static void new_tab(WinGuiSeat *wgs, unsigned id)
{
    Conf *conf = conf_new();
    char *name = NULL;

    if (id == IDM_DUPSESS) {
        conf_copy_into(conf, wgs->conf);
        if (WUI(wgs)->session_name)
            name = dupstr(WUI(wgs)->session_name);
    } else if (id >= IDM_SAVED_MIN && id < IDM_SAVED_MAX) {
        unsigned int sessno = ((id - IDM_SAVED_MIN) / MENU_SAVED_STEP) + 1;
        if (sessno >= (unsigned)sesslist.nsessions ||
            !do_defaults(sesslist.sessions[sessno], conf)) {
            conf_free(conf);
            return;
        }
        name = dupstr(sesslist.sessions[sessno]);
        if (!conf_launchable(conf)) {
            /* (it needs a host name: let the user give it one) */
            char *launched;
            bool ok = wui_do_config(wgs->term_hwnd, conf, name, &launched);
            sfree(name);
            name = launched;
            if (!ok) {
                conf_free(conf);
                return;
            }
        }
    } else /* IDM_NEWSESS */ {
        do_defaults(NULL, conf);
        if (!wui_do_config(wgs->term_hwnd, conf, NULL, &name)) {
            conf_free(conf);
            return;
        }
    }

    prepare_session(conf);
    WuiSeat *ws = open_session(conf);
    set_session_name(ws, name);
    session_touch(name);
}

/* A host name typed in to connect to (see wui_cb_quick_connect) */
static char *quick_target;
static bool quick_new_window;

void wui_cb_quick_connect(void *ctx, const wchar_t *target, bool new_window)
{
    sfree(quick_target);
    quick_target = dup_wc_to_mb(DEFAULT_CODEPAGE, target, "?");
    quick_new_window = new_window;
    /* (later, as for the menus' commands) */
    wui_cb_menu_command(ctx, IDM_QUICK);
}

static void quick_connect(WinGuiSeat *wgs)
{
    char *target = quick_target;
    quick_target = NULL;
    if (!target)
        return;
    Conf *conf = conf_new();
    do_defaults(NULL, conf);
    char *err = session_quick(conf, target);
    sfree(target);
    if (!err && !conf_launchable(conf))
        err = dupstr("There's no host name to connect to");
    if (err) {
        MessageBox(wgs->term_hwnd, err, appname, MB_OK | MB_ICONERROR);
        sfree(err);
        conf_free(conf);
        return;
    }
    prepare_session(conf);
    if (quick_new_window) {
        launch_conf = conf;
        launch_new_session(wgs, IDM_DUPSESS);
        launch_conf = NULL;
        conf_free(conf);
    } else {
        open_session(conf);
    }
}

/* A copy of a session, in a new pane beside (or below) it in its tab,
 * as Windows Terminal does */
static void split_session(WuiSeat *ws, bool side_by_side)
{
    Conf *conf = conf_copy(ws->wgs.conf);
    char *name = ws->session_name ? dupstr(ws->session_name) : NULL;
    prepare_session(conf);
    split_beside = ws->tterm;
    split_side_by_side = side_by_side;
    WuiSeat *nws = open_session(conf);
    split_beside = NULL;
    set_session_name(nws, name);
}

/* Open a saved session in a new window (from the config box) */
void wui_launch_session_window(const char *session)
{
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return;
    wchar_t *wname = dup_mb_to_wc(CP_ACP, session);
    wchar_t *cl = dupwcscat(L"\"", exe, L"\" ",
                            restricted_acl() ? L"&R@" : L"@", wname);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(exe, cl, NULL, NULL, false, NORMAL_PRIORITY_CLASS,
                       NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    sfree(cl);
    sfree(wname);
}

static void reconfigure(WinGuiSeat *wgs)
{
    Conf *prev_conf;
    int init_lvl = 1;
    bool reconfig_result;
    int resize_action;

    if (wgs->reconfiguring)
        return;
    else
        wgs->reconfiguring = true;

    /*
     * (term_pre_reconfig puts the window title as it is now in the
     * settings, as the classic front end shows it there to be edited.
     * Here, they'd then be saved like that, by Apply and save, and the
     * session would have that title, "host - PuTTY" or whatever the
     * server had set, for good. So it keeps its own, if any; and as
     * the terminal compares the new one with that, not changing it
     * doesn't change the title.)
     */
    char *own_title = dupstr(conf_get_str(wgs->conf, CONF_wintitle));
    term_pre_reconfig(wgs->term, wgs->conf);
    conf_set_str(wgs->conf, CONF_wintitle, own_title);
    sfree(own_title);
    prev_conf = conf_copy(wgs->conf);

    char *renamed = NULL;
    reconfig_result = wui_do_reconfig(
        wgs->term_hwnd, wgs->conf,
        wgs->backend ? backend_cfg_info(wgs->backend) : 0,
        WUI(wgs)->session_name, wgs->session_closed && !wgs->backend,
        &renamed);
    wgs->reconfiguring = false;
    if (renamed && !*renamed) {
        /* (its saved session's been deleted) */
        sfree(renamed);
        set_session_name(WUI(wgs), NULL);
    } else if (renamed) {
        set_session_name(WUI(wgs), renamed);
    }
    /* (its title may have been set, or unset, in its settings) */
    update_tab_label(WUI(wgs));
    if (!reconfig_result) {
        conf_free(prev_conf);
        return;
    }

    conf_cache_data(wgs);

    resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    {
        /* Gracefully unzoom if necessary */
        if (WUI(wgs) == active && win_state != WUI_STATE_NORMAL &&
            win_state != WUI_STATE_MINIMISED &&
            resize_action == RESIZE_DISABLED)
            wui_set_state(host, WUI_STATE_NORMAL);
    }

    /* Pass new config data to the logging module */
    log_reconfig(wgs->logctx, wgs->conf);

    sfree(wgs->logpal);
    /*
     * Flush the line discipline's edit buffer in the
     * case where local editing has just been disabled.
     */
    if (wgs->ldisc) {
        ldisc_configure(wgs->ldisc, wgs->conf);
        ldisc_echoedit_update(wgs->ldisc);
    }

    if (conf_get_bool(wgs->conf, CONF_system_colour) !=
        conf_get_bool(prev_conf, CONF_system_colour))
        term_notify_palette_changed(wgs->term);

    /* Pass new config data to the terminal */
    term_reconfig(wgs->term, wgs->conf);
    setup_clipboards(wgs->term, wgs->conf);

    /* Reinitialise the colour palette, in case the terminal
     * just read new settings out of Conf */
    wgs->logpal = NULL;
    init_palette(wgs);

    /* Pass new config data to the back end */
    if (wgs->backend)
        backend_reconfig(wgs->backend, wgs->conf);

    /* Screen size changed ? */
    if (conf_get_int(wgs->conf, CONF_height) !=
        conf_get_int(prev_conf, CONF_height) ||
        conf_get_int(wgs->conf, CONF_width) !=
        conf_get_int(prev_conf, CONF_width) ||
        conf_get_int(wgs->conf, CONF_savelines) !=
        conf_get_int(prev_conf, CONF_savelines) ||
        resize_action == RESIZE_FONT ||
        (resize_action == RESIZE_EITHER && is_zoomed(wgs)) ||
        resize_action == RESIZE_DISABLED)
        term_size(wgs->term, conf_get_int(wgs->conf, CONF_height),
                  conf_get_int(wgs->conf, CONF_width),
                  conf_get_int(wgs->conf, CONF_savelines));

    /* Enable or disable the scroll bar, etc */
    {
        if (WUI(wgs) == active)
            apply_window_settings(wgs);
        if (conf_get_bool(wgs->conf, CONF_scrollbar) !=
            conf_get_bool(prev_conf, CONF_scrollbar) ||
            conf_get_bool(wgs->conf, CONF_scrollbar_in_fullscreen) !=
            conf_get_bool(prev_conf, CONF_scrollbar_in_fullscreen) ||
            resize_action != conf_get_int(prev_conf, CONF_resize_action)) {
            update_scrollbar_visibility(wgs);
            init_lvl = 2;
        }
    }

    /* Oops */
    if (resize_action == RESIZE_DISABLED && WUI(wgs) == active &&
        (win_state == WUI_STATE_MAXIMISED ||
         win_state == WUI_STATE_FULLSCREEN)) {
        wui_set_state(host, WUI_STATE_NORMAL);
        init_lvl = 2;
    }

    {
        FontSpec *font = conf_get_fontspec(wgs->conf, CONF_font);
        FontSpec *prev_font = conf_get_fontspec(prev_conf,
                                                CONF_font);

        if (!strcmp(font->name, prev_font->name) ||
            !strcmp(conf_get_str(wgs->conf, CONF_line_codepage),
                    conf_get_str(prev_conf, CONF_line_codepage)) ||
            font->isbold != prev_font->isbold ||
            font->height != prev_font->height ||
            font->charset != prev_font->charset ||
            conf_get_int(wgs->conf, CONF_font_quality) !=
            conf_get_int(prev_conf, CONF_font_quality) ||
            conf_get_int(wgs->conf, CONF_vtmode) !=
            conf_get_int(prev_conf, CONF_vtmode) ||
            conf_get_int(wgs->conf, CONF_bold_style) !=
            conf_get_int(prev_conf, CONF_bold_style) ||
            resize_action == RESIZE_DISABLED ||
            resize_action == RESIZE_EITHER ||
            resize_action != conf_get_int(prev_conf,
                                          CONF_resize_action))
            init_lvl = 2;
    }

    invalidate_all(wgs);
    reset_window(wgs, init_lvl);

    conf_free(prev_conf);
}

static void do_command(WinGuiSeat *wgs, unsigned id)
{
    HWND hwnd = wgs->term_hwnd;

    if (!wgs->term)
        return;

    frontend_busy_enter();

    switch (id) {
      case IDM_SHOWLOG:
        show_log(WUI(wgs));
        break;
      case IDM_NEWSESS:
      case IDM_DUPSESS:
        new_tab(wgs, id);
        break;
      case IDM_NEWWIN:
        launch_new_session(wgs, id);
        break;
      case IDM_QUICK:
        quick_connect(wgs);
        break;
      case IDM_RESTART:
        if (wgs->backend || !wgs->session_closed) {
            /* (still connected, or connecting: ask first, as it'll be
             * disconnected) */
            bool restart;
            show_mouseptr(wgs, true);
            frontend_busy_enter();
            restart = show_message(
                "Restart Session", "Are you sure you want to restart this "
                "session? It's still connected, and will be disconnected "
                "first.", L"Restart", true);
            frontend_busy_leave();
            if (!restart || !find_seat(WUI(wgs)->id))
                break;
            close_session(wgs);
        }
        lp_eventlog(&wgs->logpolicy, "----- Session restarted -----");
        term_pwron(wgs->term, false);
        start_backend(wgs);
        break;
      case IDM_RECONF:
        reconfigure(wgs);
        break;
      case IDM_COPYALL:
        /* (not into the tab's pool: see pool_copied) */
        WUI(wgs)->pool_skip = true;
        term_copyall(wgs->term, clips_system, lenof(clips_system));
        WUI(wgs)->pool_skip = false;
        break;
      case IDM_FIND:
        wui_term_show_search(WUI(wgs)->tterm);
        break;
      case IDM_SENDFILE:
        send_file(WUI(wgs));
        break;
      case IDM_RECVFILE:
        receive_file(WUI(wgs));
        break;
      case IDM_COPY:
        term_request_copy(wgs->term, clips_system, lenof(clips_system));
        break;
      case IDM_PASTE:
        term_request_paste(wgs->term, CLIP_SYSTEM);
        break;
      case IDM_CLRSB:
        term_clrsb(wgs->term);
        break;
      case IDM_RESET:
        term_pwron(wgs->term, true);
        if (wgs->ldisc)
            ldisc_echoedit_update(wgs->ldisc);
        break;
      case IDM_ABOUT:
        show_about(hwnd);
        break;
      case IDM_HELP:
        wui_show_help(hwnd, NULL);
        break;
      case IDM_FULLSCREEN:
        if (conf_get_int(wgs->conf, CONF_resize_action) != RESIZE_DISABLED)
            flip_full_screen(wgs);
        break;
      case IDM_TABLEFT:
        wui_term_move(WUI(wgs)->tterm, -1);
        break;
      case IDM_TABRIGHT:
        wui_term_move(WUI(wgs)->tterm, +1);
        break;
      case IDM_RENAMETAB:
        wui_term_rename_tab(WUI(wgs)->tterm);
        break;
      case IDM_POOL_SESSION:
      case IDM_POOL_COMMON:
        /* (the selection, copied as Copy does, and kept there as well:
         * see pool_copied) */
        WUI(wgs)->pool_target = id == IDM_POOL_SESSION ?
            WUI_POOL_SESSION : WUI_POOL_COMMON;
        term_request_copy(wgs->term, clips_system, lenof(clips_system));
        WUI(wgs)->pool_target = 0;
        break;
      case IDM_POOL_PASTE:
        show_pool(WUI(wgs));
        break;
      case IDM_CLOSETAB:
        /* (Which may free wgs, so not from in here) */
        PostMessage(msg_hwnd, WM_WUI_CLOSE_TAB, WUI(wgs)->id, 0);
        break;
      case IDM_CLOSEPANE:
        PostMessage(msg_hwnd, WM_WUI_CLOSE_PANE, WUI(wgs)->id, 0);
        break;
      case IDM_SPLITRIGHT:
      case IDM_SPLITDOWN:
        split_session(WUI(wgs), id == IDM_SPLITRIGHT);
        break;
      default:
        if (id >= IDM_SAVED_MIN && id < IDM_SAVED_MAX) {
            /* (The saved sessions button's menu marks them with
             * WUI_MENU_NEW_WINDOW, if Shift was held down) */
            if (id & WUI_MENU_NEW_WINDOW)
                launch_new_session(wgs, id & ~WUI_MENU_NEW_WINDOW);
            else
                new_tab(wgs, id);
        }
        if (id >= IDM_SPECIAL_MIN && id <= IDM_SPECIAL_MAX) {
            int i = (id - IDM_SPECIAL_MIN) / 0x10;
            /*
             * Ensure we haven't been sent a bogus command which would
             * cause us to reference invalid memory and crash.
             */
            if (i >= wgs->n_specials)
                break;
            if (wgs->backend)
                backend_special(wgs->backend, wgs->specials[i].code,
                                wgs->specials[i].arg);
        }
    }

    frontend_busy_leave();
}

/* ----------------------------------------------------------------------
 * Mouse input.
 */

#define TO_CHR_X(x) ((((x)<0 ? (x)-wgs->font_width+1 :                  \
                       (x))-wgs->offset_width) / wgs->font_width)
#define TO_CHR_Y(y) ((((y)<0 ? (y)-wgs->font_height+1 :                 \
                       (y))-wgs->offset_height) / wgs->font_height)

static void mouse_wheel(WinGuiSeat *wgs, const WuiPointerEvent *ev)
{
    bool shift_pressed = ev->shift, control_pressed = ev->ctrl;

    wgs->wheel_accumulator += ev->wheel_delta;

    /* process events when the threshold is reached */
    while (abs(wgs->wheel_accumulator) >= WHEEL_DELTA) {
        int b;

        /* reduce amount for next time */
        if (wgs->wheel_accumulator > 0) {
            b = ev->wheel_horizontal ? MBT_WHEEL_RIGHT : MBT_WHEEL_UP;
            wgs->wheel_accumulator -= WHEEL_DELTA;
        } else if (wgs->wheel_accumulator < 0) {
            b = ev->wheel_horizontal ? MBT_WHEEL_LEFT : MBT_WHEEL_DOWN;
            wgs->wheel_accumulator += WHEEL_DELTA;
        } else
            break;

        if (wgs->send_raw_mouse &&
            !(conf_get_bool(wgs->conf, CONF_mouse_override) &&
              shift_pressed)) {
            /* send a mouse-down followed by a mouse up */
            term_mouse(wgs->term, b, translate_button(wgs, b),
                       MA_CLICK, TO_CHR_X(ev->x), TO_CHR_Y(ev->y),
                       shift_pressed, control_pressed, is_alt_pressed());
        } else if (!ev->wheel_horizontal) {
            /* trigger a scroll */
            term_scroll(wgs->term, 0,
                        b == MBT_WHEEL_UP ?
                        -wgs->term->rows / 2 : wgs->term->rows / 2);
        }
    }
}

void wui_cb_pointer(void *ctx, const WuiPointerEvent *ev)
{
    WuiSeat *ws = (WuiSeat *)ctx;
    WinGuiSeat *wgs = &ws->wgs;
    Mouse_Button button;

    if (!wgs->term)
        return;

    switch (ev->kind) {
      case WUI_POINTER_WHEEL:
        mouse_wheel(wgs, ev);
        return;

      case WUI_POINTER_MOVE:
        /*
         * Pointer events can arrive even if the mouse hasn't moved.
         * Don't unhide the mouse pointer in that case.
         */
        if (ev->x != wgs->last_wm_mousemove_lParam ||
            ev->y != wgs->last_wm_mousemove_wParam) {
            show_mouseptr(wgs, true);
            wgs->last_wm_mousemove_lParam = ev->x;
            wgs->last_wm_mousemove_wParam = ev->y;
        }
        /*
         * Add the mouse position and message time to the random
         * number noise.
         */
        noise_ultralight(NOISE_SOURCE_MOUSEPOS,
                         ((unsigned)ev->y << 16) | (ev->x & 0xFFFF));

        if (ev->left_down || ev->middle_down || ev->right_down) {
            Mouse_Button b;
            if (ev->left_down)
                b = MBT_LEFT;
            else if (ev->middle_down)
                b = MBT_MIDDLE;
            else
                b = MBT_RIGHT;
            term_mouse(wgs->term, b, translate_button(wgs, b), MA_DRAG,
                       TO_CHR_X(ev->x), TO_CHR_Y(ev->y), ev->shift,
                       ev->ctrl, is_alt_pressed());
        } else {
            term_mouse(wgs->term, MBT_NOTHING, MBT_NOTHING, MA_MOVE,
                       TO_CHR_X(ev->x), TO_CHR_Y(ev->y), false,
                       false, false);
        }
        return;

      case WUI_POINTER_PRESS:
      case WUI_POINTER_RELEASE:
        break;
    }

    if (ev->kind == WUI_POINTER_PRESS && ev->button == WUI_BUTTON_RIGHT &&
        (ev->ctrl ||
         conf_get_int(wgs->conf, CONF_mouse_is_xterm) == MOUSE_WINDOWS)) {
        /* Just in case this happened in mid-select */
        term_cancel_selection_drag(wgs->term);

        show_mouseptr(wgs, true);    /* make sure pointer is visible */
        wui_show_context_menu(ws->tterm, ev->x, ev->y);
        return;
    }

    switch (ev->button) {
      case WUI_BUTTON_LEFT: button = MBT_LEFT; break;
      case WUI_BUTTON_MIDDLE: button = MBT_MIDDLE; break;
      case WUI_BUTTON_RIGHT: button = MBT_RIGHT; break;
      default: return;
    }
    show_mouseptr(wgs, true);

    /*
     * Special case: in full-screen mode, if the left button is
     * clicked in the very top left corner of the screen, we put up
     * the menu instead of doing selection, since there's no title
     * bar to get it from.
     */
    if (is_full_screen(wgs) && ev->kind == WUI_POINTER_PRESS &&
        button == MBT_LEFT) {
        POINT pt;
        bool mouse_on_hotspot = false;
        GetCursorPos(&pt);
        if (p_GetMonitorInfoA && p_MonitorFromPoint) {
            HMONITOR mon = p_MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
            if (mon != NULL) {
                MONITORINFO mi;
                mi.cbSize = sizeof(MONITORINFO);
                p_GetMonitorInfoA(mon, &mi);
                if (mi.rcMonitor.left == pt.x && mi.rcMonitor.top == pt.y)
                    mouse_on_hotspot = true;
            }
        } else if (pt.x == 0 && pt.y == 0) {
            mouse_on_hotspot = true;
        }
        if (mouse_on_hotspot) {
            wui_show_context_menu(ws->tterm, ev->x, ev->y);
            return;
        }
    }

    if (ev->kind == WUI_POINTER_PRESS) {
        click(wgs, button, TO_CHR_X(ev->x), TO_CHR_Y(ev->y),
              ev->shift, ev->ctrl, is_alt_pressed(), (int)ev->time_ms);
    } else {
        term_mouse(wgs->term, button, translate_button(wgs, button),
                   MA_RELEASE, TO_CHR_X(ev->x), TO_CHR_Y(ev->y),
                   ev->shift, ev->ctrl, is_alt_pressed());
    }
}

/*
 * Move the system caret. (We maintain one, even though it's
 * invisible, for the benefit of blind people: apparently some
 * helper software tracks the system caret, so we should arrange to
 * have one.)
 */
static void wintw_set_cursor_pos(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int cx, cy;

    if (!wgs->term->has_focus) return;

    /*
     * Avoid gratuitously re-updating the cursor position and IMM
     * window if there's no actual change required.
     */
    cx = x * wgs->font_width + wgs->offset_width;
    cy = y * wgs->font_height + wgs->offset_height;
    if (cx == wgs->caret_x && cy == wgs->caret_y)
        return;
    wgs->caret_x = cx;
    wgs->caret_y = cy;

    sys_cursor_update(wgs);
}

static void sys_cursor_update(WinGuiSeat *wgs)
{
    COMPOSITIONFORM cf;
    HIMC hIMC;
    HWND focus;

    if (!wgs->term || !wgs->term->has_focus) return;

    if (wgs->caret_x < 0 || wgs->caret_y < 0)
        return;

    /* Keyboard focus is on the XAML framework's input window, which
     * covers the client area, so terminal-area coordinates are also
     * coordinates in that window. */
    focus = GetFocus();
    if (!focus || !is_terminal_hwnd(wgs, focus))
        return;

    SetCaretPos(wgs->caret_x, wgs->caret_y);

    hIMC = ImmGetContext(focus);
    if (hIMC) {
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = wgs->caret_x;
        cf.ptCurrentPos.y = wgs->caret_y;
        ImmSetCompositionWindow(hIMC, &cf);
        ImmSetCompositionFont(hIMC, &wgs->lfont);
        ImmReleaseContext(focus, hIMC);
    }
}

/* ----------------------------------------------------------------------
 * Drawing.
 */

static void fill_rect(WinGuiSeat *wgs, int x, int y, int w, int h,
                      COLORREF c)
{
    wui_fill_rect(WUI(wgs)->tterm, x, y, w, h, c);
}

static int line_thickness(WinGuiSeat *wgs)
{
    int t = wgs->dpi_info.cur_dpi.y / 96;
    return t < 1 ? 1 : t;
}

static void draw_horizontal_line_on_text(
    WinGuiSeat *wgs, int y, int lattr, RECT line_box, COLORREF colour)
{
    if (lattr == LATTR_TOP || lattr == LATTR_BOT) {
        y *= 2;
        if (lattr == LATTR_BOT)
            y -= wgs->font_height;
    }

    if (!(0 <= y && y < wgs->font_height))
        return;

    int t = line_thickness(wgs);
    if (y + t > wgs->font_height)
        t = wgs->font_height - y;
    fill_rect(wgs, line_box.left, line_box.top + y,
              line_box.right - line_box.left, t, colour);
}

/*
 * Convert characters that are to be displayed in the font's own
 * encoding into Unicode, since DirectWrite only speaks Unicode.
 */
static int direct_font_to_unicode(WinGuiSeat *wgs, const wchar_t *text,
                                  int len, wchar_t *wbuf, int *dx,
                                  int char_width)
{
    int nlen = 0;
    int cp = ((text[0] & CSET_MASK) == CSET_OEMCP ? CP_OEMCP :
              wgs->ucsdata.font_codepage > 0 ? wgs->ucsdata.font_codepage :
              CP_ACP);

    for (int mptr = 0; mptr < len; mptr++) {
        char mb[2];
        int mblen = 1;
        wbuf[nlen] = 0xFFFD;
        dx[nlen] = char_width;
        mb[0] = text[mptr] & 0xFF;
        if (wgs->ucsdata.dbcs_screenfont && mptr + 1 < len &&
            IsDBCSLeadByteEx(cp, (BYTE)mb[0])) {
            mb[1] = text[mptr+1] & 0xFF;
            mblen = 2;
            dx[nlen] += char_width;
            mptr++;
        }
        MultiByteToWideChar(cp, MB_USEGLYPHCHARS, mb, mblen, wbuf+nlen, 1);
        nlen++;
    }
    return nlen;
}

/*
 * Draw a line of text in the window, at given character
 * coordinates, in given attributes.
 *
 * We are allowed to fiddle with the contents of `text'.
 */
static void do_text_internal(
    WinGuiSeat *wgs, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    COLORREF fg, bg, t;
    int nfg, nbg;
    RECT line_box;
    bool force_manual_underline = false;
    int fnt_width, char_width;
    int text_adjust = 0;
    bool is_cursor = false;
    float scale_x = 1, scale_y = 1;
    int *dx;
    wchar_t *wbuf;
    int wlen;
    WuiTextRequest req;

    lattr &= LATTR_MODE;

    char_width = fnt_width = wgs->font_width * (1 + (lattr != LATTR_NORM));

    if (attr & ATTR_WIDE)
        char_width *= 2;

    /* Only want the left half of double width lines */
    if (lattr != LATTR_NORM && x*2 >= wgs->term->cols)
        return;

    x *= fnt_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    if ((attr & ATTR_ACTCURS) &&
        (wgs->cursor_type == CURSOR_BLOCK || wgs->term->big_cursor)) {
        truecolour.fg = truecolour.bg = optionalrgb_none;
        attr &= ~(ATTR_REVERSE|ATTR_BLINK|ATTR_COLOURS|ATTR_DIM);
        /* cursor fg and bg */
        attr |= (260 << ATTR_FGSHIFT) | (261 << ATTR_BGSHIFT);
        is_cursor = true;
    }

    if (wgs->vtmode == VT_POORMAN && lattr != LATTR_NORM) {
        /* Assume a poorman font is borken in other ways too. */
        lattr = LATTR_WIDE;
    }
    switch (lattr) {
      case LATTR_NORM:
        break;
      case LATTR_WIDE:
        scale_x = 2;
        break;
      default:
        scale_x = scale_y = 2;
        break;
    }
    if (attr & ATTR_NARROW)
        scale_x /= 2;

#ifdef USES_VTLINE_HACK
    /* Special hack for the VT100 linedraw glyphs. */
    if (text[0] >= 0x23BA && text[0] <= 0x23BD) {
        switch ((unsigned char) (text[0])) {
          case 0xBA:
            text_adjust = -2 * wgs->font_height / 5;
            break;
          case 0xBB:
            text_adjust = -1 * wgs->font_height / 5;
            break;
          case 0xBC:
            text_adjust = wgs->font_height / 5;
            break;
          case 0xBD:
            text_adjust = 2 * wgs->font_height / 5;
            break;
        }
        if (lattr == LATTR_TOP || lattr == LATTR_BOT)
            text_adjust *= 2;
        text[0] = wgs->ucsdata.unitab_xterm['q'];
        if (attr & ATTR_UNDER) {
            attr &= ~ATTR_UNDER;
            force_manual_underline = true;
        }
    }
#endif

    /* Anything left as an original character set is unprintable. */
    if (DIRECT_CHAR(text[0]) &&
        (len < 2 || !IS_SURROGATE_PAIR(text[0], text[1]))) {
        int i;
        for (i = 0; i < len; i++)
            text[i] = 0xFFFD;
    }

    nfg = ((attr & ATTR_FGMASK) >> ATTR_FGSHIFT);
    nbg = ((attr & ATTR_BGMASK) >> ATTR_BGSHIFT);
    if (attr & ATTR_REVERSE) {
        struct optionalrgb trgb;

        t = nfg;
        nfg = nbg;
        nbg = t;

        trgb = truecolour.fg;
        truecolour.fg = truecolour.bg;
        truecolour.bg = trgb;
    }
    if (wgs->bold_colours && (attr & ATTR_BOLD) && !is_cursor) {
        if (nfg < 16) nfg |= 8;
        else if (nfg >= 256) nfg |= 1;
    }
    if (wgs->bold_colours && (attr & ATTR_BLINK)) {
        if (nbg < 16) nbg |= 8;
        else if (nbg >= 256) nbg |= 1;
    }
    if (truecolour.fg.enabled)
        fg = RGB(truecolour.fg.r, truecolour.fg.g, truecolour.fg.b);
    else
        fg = wgs->colours[nfg];

    if (truecolour.bg.enabled)
        bg = RGB(truecolour.bg.r, truecolour.bg.g, truecolour.bg.b);
    else
        bg = wgs->colours[nbg];

    if (attr & ATTR_DIM) {
        fg = RGB(GetRValue(fg) * 2 / 3,
                 GetGValue(fg) * 2 / 3,
                 GetBValue(fg) * 2 / 3);
    }

    line_box.left = x;
    line_box.top = y;
    line_box.right = x + char_width * len;
    line_box.bottom = y + wgs->font_height;
    /* adjust line_box.right for SURROGATE PAIR & VARIATION SELECTOR */
    {
        int i;
        int rc_width = 0;
        for (i = 0; i < len ; i++) {
            if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                i++;
            } else if (i+1 < len && IS_SURROGATE_PAIR(text[i], text[i+1])) {
                rc_width += char_width;
                i++;
            } else if (IS_LOW_VARSEL(text[i])) {
                /* do nothing */
            } else {
                rc_width += char_width;
            }
        }
        line_box.right = line_box.left + rc_width;
    }

    /* Only want the left half of double width lines */
    if (line_box.right > wgs->font_width*wgs->term->cols+wgs->offset_width)
        line_box.right = wgs->font_width*wgs->term->cols+wgs->offset_width;

    /* Erase the background, unless we're overprinting a combining
     * character onto the cell. */
    if (!(attr & TATTR_COMBINING))
        fill_rect(wgs, line_box.left, line_box.top,
                  line_box.right - line_box.left,
                  line_box.bottom - line_box.top, bg);

    dx = snewn(2 * len + 1, int);
    wbuf = snewn(2 * len + 1, wchar_t);

    if (DIRECT_FONT(text[0])) {
        /* Characters in the font's own encoding */
        wlen = direct_font_to_unicode(wgs, text, len, wbuf, dx, char_width);
    } else {
        /* And 'normal' unicode characters. Only the last char of a
         * SURROGATE PAIR or VARIATION sequence has an advance. */
        int i;
        for (i = 0; i < len; i++) {
            wbuf[i] = text[i];
            dx[i] = char_width;
            if (i+1 < len && IS_HIGH_VARSEL(text[i], text[i+1])) {
                if (i > 0) dx[i-1] = 0;
                dx[i] = 0;
                i++;
                wbuf[i] = text[i];
                dx[i] = char_width;
            } else if (i+1 < len && IS_SURROGATE_PAIR(text[i], text[i+1])) {
                dx[i] = 0;
                i++;
                wbuf[i] = text[i];
                dx[i] = char_width;
            } else if (IS_LOW_VARSEL(text[i])) {
                if (i > 0) dx[i-1] = 0;
                dx[i] = char_width;
            }
        }
        wlen = len;
    }

    memset(&req, 0, sizeof(req));
    req.text = wbuf;
    req.len = wlen;
    req.dx = dx;
    req.x = x;
    req.y = y - wgs->font_height * (lattr == LATTR_BOT) + text_adjust;
    req.clip = line_box;
    req.fg = fg;
    req.bold = (wgs->bold_font_mode == BOLD_FONT && (attr & ATTR_BOLD));
    req.shadow_bold = (wgs->bold_font_mode == BOLD_SHADOW &&
                       (attr & ATTR_BOLD));
    req.centre = wgs->font_varpitch;
    req.scale_x = scale_x;
    req.scale_y = scale_y;
    if (wlen > 0)
        wui_draw_text(WUI(wgs)->tterm, &req);

    sfree(dx);
    sfree(wbuf);

    if (lattr != LATTR_TOP &&
        (force_manual_underline || (attr & ATTR_UNDER)))
        draw_horizontal_line_on_text(wgs, wgs->descent, lattr, line_box, fg);

    if (attr & ATTR_STRIKE)
        draw_horizontal_line_on_text(wgs, wgs->font_strikethrough_y, lattr,
                                     line_box, fg);
}

/*
 * Wrapper that handles combining characters.
 */
static void wintw_draw_text(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (attr & TATTR_COMBINING) {
        unsigned long a = 0;
        int len0 = 1;
        /* don't divide SURROGATE PAIR and VARIATION SELECTOR */
        if (len >= 2 && IS_SURROGATE_PAIR(text[0], text[1]))
            len0 = 2;
        if (len-len0 >= 1 && IS_LOW_VARSEL(text[len0])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(wgs, x, y, text, len0+1, attr, lattr, truecolour);
            text += len0+1;
            len -= len0+1;
            a = TATTR_COMBINING;
        } else if (len-len0 >= 2 && IS_HIGH_VARSEL(text[len0], text[len0+1])) {
            attr &= ~TATTR_COMBINING;
            do_text_internal(wgs, x, y, text, len0+2, attr, lattr, truecolour);
            text += len0+2;
            len -= len0+2;
            a = TATTR_COMBINING;
        } else {
            attr &= ~TATTR_COMBINING;
        }

        while (len--) {
            if (len >= 1 && IS_SURROGATE_PAIR(text[0], text[1])) {
                do_text_internal(wgs, x, y, text, 2, attr | a, lattr,
                                 truecolour);
                len--;
                text++;
            } else
                do_text_internal(wgs, x, y, text, 1, attr | a, lattr,
                                 truecolour);

            text++;
            a = TATTR_COMBINING;
        }
    } else
        do_text_internal(wgs, x, y, text, len, attr, lattr, truecolour);
}

static void wintw_draw_cursor(
    TermWin *tw, int x, int y, wchar_t *text, int len,
    unsigned long attr, int lattr, truecolour truecolour)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int fnt_width;
    int char_width;
    int ctype = wgs->cursor_type;
    int thick = line_thickness(wgs);

    lattr &= LATTR_MODE;

    if ((attr & ATTR_ACTCURS) &&
        (ctype == CURSOR_BLOCK || wgs->term->big_cursor)) {
        if (*text != UCSWIDE) {
            win_draw_text(tw, x, y, text, len, attr, lattr, truecolour);
            return;
        }
        ctype = CURSOR_VERTICAL_LINE;
        attr |= ATTR_RIGHTCURS;
    }

    fnt_width = char_width = wgs->font_width * (1 + (lattr != LATTR_NORM));
    if (attr & ATTR_WIDE)
        char_width *= 2;
    x *= fnt_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    if ((attr & ATTR_PASCURS) &&
        (ctype == CURSOR_BLOCK || wgs->term->big_cursor)) {
        /* Hollow box */
        COLORREF c = wgs->colours[261];
        fill_rect(wgs, x, y, char_width, thick, c);
        fill_rect(wgs, x, y + wgs->font_height - thick, char_width, thick, c);
        fill_rect(wgs, x, y, thick, wgs->font_height, c);
        fill_rect(wgs, x + char_width - thick, y, thick, wgs->font_height, c);
    } else if ((attr & (ATTR_ACTCURS | ATTR_PASCURS)) &&
               ctype != CURSOR_BLOCK) {
        int startx, starty, dx, dy, length, i;
        if (ctype == CURSOR_UNDERLINE) {
            startx = x;
            starty = y + wgs->descent;
            dx = 1;
            dy = 0;
            length = char_width;
        } else /* ctype == CURSOR_VERTICAL_LINE */ {
            int xadjust = 0;
            if (attr & ATTR_RIGHTCURS)
                xadjust = char_width - thick;
            startx = x + xadjust;
            starty = y;
            dx = 0;
            dy = 1;
            length = wgs->font_height;
        }
        if (attr & ATTR_ACTCURS) {
            if (dx)
                fill_rect(wgs, startx, starty, length, thick,
                          wgs->colours[261]);
            else
                fill_rect(wgs, startx, starty, thick, length,
                          wgs->colours[261]);
        } else {
            for (i = 0; i < length; i++) {
                if (i % 2 == 0)
                    fill_rect(wgs, startx, starty, 1, 1, wgs->colours[261]);
                startx += dx;
                starty += dy;
            }
        }
    }
}

static void wintw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);

    x *= wgs->font_width;
    y *= wgs->font_height;
    x += wgs->offset_width;
    y += wgs->offset_height;

    HICON icon = WUI(wgs)->trust_icon;
    if (icon != INVALID_HANDLE_VALUE && icon)
        wui_draw_icon(WUI(wgs)->tterm, icon, x, y,
                      wgs->font_width * 2, wgs->font_height);
}

/* This function gets the actual width of a character in the normal font.
 */
static int wintw_char_width(TermWin *tw, int uc)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int ibuf;

    /* If the font max is the same as the font ave width then this
     * function is a no-op.
     */
    if (!wgs->font_dualwidth) return 1;

    switch (uc & CSET_MASK) {
      case CSET_ASCII:
        uc = wgs->ucsdata.unitab_line[uc & 0xFF];
        break;
      case CSET_LINEDRW:
        uc = wgs->ucsdata.unitab_xterm[uc & 0xFF];
        break;
      case CSET_SCOACS:
        uc = wgs->ucsdata.unitab_scoacs[uc & 0xFF];
        break;
    }
    if (DIRECT_FONT(uc)) {
        wchar_t wc;
        char c;
        int cp;

        if (wgs->ucsdata.dbcs_screenfont) return 1;

        /* Speedup, I know of no font where ascii is the wrong width */
        if ((uc&~CSET_MASK) >= ' ' && (uc&~CSET_MASK)<= '~')
            return 1;

        if ((uc & CSET_MASK) == CSET_ACP)
            cp = (wgs->ucsdata.font_codepage > 0 ?
                  wgs->ucsdata.font_codepage : CP_ACP);
        else if ((uc & CSET_MASK) == CSET_OEMCP)
            cp = CP_OEMCP;
        else
            return 0;

        c = uc & 0xFF;
        if (MultiByteToWideChar(cp, MB_USEGLYPHCHARS, &c, 1, &wc, 1) != 1)
            return 0;
        uc = wc;
    } else {
        /* Speedup, I know of no font where ascii is the wrong width */
        if (uc >= ' ' && uc <= '~') return 1;
    }

    ibuf = wui_char_width(WUI(wgs)->tterm, uc);
    if (ibuf == 0)
        return 0;

    ibuf += wgs->font_width / 2 -1;
    ibuf /= wgs->font_width;

    return ibuf;
}

DECL_WINDOWS_FUNCTION(static, BOOL, FlashWindowEx, (PFLASHWINFO));
DECL_WINDOWS_FUNCTION(static, BOOL, ToUnicodeEx,
                      (UINT, UINT, const BYTE *, LPWSTR, int, UINT, HKL));
DECL_WINDOWS_FUNCTION(static, BOOL, PlaySoundW, (LPCWSTR, HMODULE, DWORD));
DECL_WINDOWS_FUNCTION(static, BOOL, PlaySoundA, (LPCSTR, HMODULE, DWORD));

static void init_winfuncs(void)
{
    HMODULE user32_module = load_system32_dll("user32.dll");
    HMODULE winmm_module = load_system32_dll("winmm.dll");
    GET_WINDOWS_FUNCTION(user32_module, FlashWindowEx);
    GET_WINDOWS_FUNCTION(user32_module, ToUnicodeEx);
    GET_WINDOWS_FUNCTION(winmm_module, PlaySoundW);
    GET_WINDOWS_FUNCTION(winmm_module, PlaySoundA);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, GetMonitorInfoA);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, MonitorFromPoint);
    GET_WINDOWS_FUNCTION_NO_TYPECHECK(user32_module, MonitorFromWindow);
}

/*
 * Translate a WM_(SYS)?KEY(UP|DOWN) message into a string of ASCII
 * codes. Returns number of bytes used, zero to drop the message,
 * -1 to forward the message to Windows, or another negative number
 * to indicate a NUL-terminated "special" string.
 */
static int TranslateKey(WinGuiSeat *wgs, UINT message, WPARAM wParam,
                        LPARAM lParam, unsigned char *output)
{
    BYTE keystate[256];
    int scan, shift_state;
    bool left_alt = false, key_down;
    int r, i;
    unsigned char *p = output;
    int funky_type = conf_get_int(wgs->conf, CONF_funky_type);
    bool no_applic_k = conf_get_bool(wgs->conf, CONF_no_applic_k);
    bool ctrlaltkeys = conf_get_bool(wgs->conf, CONF_ctrlaltkeys);
    bool nethack_keypad = conf_get_bool(wgs->conf, CONF_nethack_keypad);
    char keypad_key = '\0';

    HKL kbd_layout = GetKeyboardLayout(0);

    r = GetKeyboardState(keystate);
    if (!r)
        memset(keystate, 0, sizeof(keystate));
    else {
        if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED)) {
            keystate[VK_RMENU] = keystate[VK_MENU];
        }


        /* Nastiness with NUMLock - Shift-NUMLock is left alone though */
        if ((funky_type == FUNKY_VT400 ||
             (funky_type <= FUNKY_LINUX && wgs->term->app_keypad_keys &&
              !no_applic_k))
            && wParam == VK_NUMLOCK && !(keystate[VK_SHIFT] & 0x80)) {

            wParam = VK_EXECUTE;

            /* UnToggle NUMLock */
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
                keystate[VK_NUMLOCK] ^= 1;
        }

        /* And write back the 'adjusted' state */
        SetKeyboardState(keystate);
    }

    /* Disable Auto repeat if required */
    if (wgs->term->repeat_off &&
        (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == KF_REPEAT)
        return 0;

    if ((HIWORD(lParam) & KF_ALTDOWN) && (keystate[VK_RMENU] & 0x80) == 0)
        left_alt = true;

    key_down = ((HIWORD(lParam) & KF_UP) == 0);

    /* Make sure Ctrl-ALT is not the same as AltGr for ToAscii unless told. */
    if (left_alt && (keystate[VK_CONTROL] & 0x80)) {
        if (ctrlaltkeys)
            keystate[VK_MENU] = 0;
        else {
            keystate[VK_RMENU] = 0x80;
            left_alt = false;
        }
    }

    scan = (HIWORD(lParam) & (KF_UP | KF_EXTENDED | 0xFF));
    shift_state = ((keystate[VK_SHIFT] & 0x80) != 0)
        + ((keystate[VK_CONTROL] & 0x80) != 0) * 2;

    /* Note if AltGr was pressed and if it was used as a compose key */
    if (!wgs->compose_state) {
        wgs->compose_keycode = 0x100;
        if (conf_get_bool(wgs->conf, CONF_compose_key)) {
            if (wParam == VK_MENU && (HIWORD(lParam) & KF_EXTENDED))
                wgs->compose_keycode = wParam;
        }
        if (wParam == VK_APPS)
            wgs->compose_keycode = wParam;
    }

    if (wParam == wgs->compose_keycode) {
        if (wgs->compose_state == 0 &&
            (HIWORD(lParam) & (KF_UP | KF_REPEAT)) == 0)
            wgs->compose_state = 1;
        else if (wgs->compose_state == 1 && (HIWORD(lParam) & KF_UP))
            wgs->compose_state = 2;
        else
            wgs->compose_state = 0;
    } else if (wgs->compose_state == 1 && wParam != VK_CONTROL)
        wgs->compose_state = 0;

    if (wgs->compose_state > 1 && left_alt)
        wgs->compose_state = 0;

    /* Sanitize the number pad if not using a PC NumPad */
    if (left_alt || (wgs->term->app_keypad_keys && !no_applic_k
                     && funky_type != FUNKY_XTERM) ||
        funky_type == FUNKY_VT400 || nethack_keypad || wgs->compose_state) {
        if ((HIWORD(lParam) & KF_EXTENDED) == 0) {
            int nParam = 0;
            switch (wParam) {
              case VK_INSERT:
                nParam = VK_NUMPAD0;
                break;
              case VK_END:
                nParam = VK_NUMPAD1;
                break;
              case VK_DOWN:
                nParam = VK_NUMPAD2;
                break;
              case VK_NEXT:
                nParam = VK_NUMPAD3;
                break;
              case VK_LEFT:
                nParam = VK_NUMPAD4;
                break;
              case VK_CLEAR:
                nParam = VK_NUMPAD5;
                break;
              case VK_RIGHT:
                nParam = VK_NUMPAD6;
                break;
              case VK_HOME:
                nParam = VK_NUMPAD7;
                break;
              case VK_UP:
                nParam = VK_NUMPAD8;
                break;
              case VK_PRIOR:
                nParam = VK_NUMPAD9;
                break;
              case VK_DELETE:
                nParam = VK_DECIMAL;
                break;
            }
            if (nParam) {
                if (keystate[VK_NUMLOCK] & 1)
                    shift_state |= 1;
                wParam = nParam;
            }
        }
    }

    /* If a key is pressed and AltGr is not active */
    if (key_down && (keystate[VK_RMENU] & 0x80) == 0 && !wgs->compose_state) {
        /* Okay, prepare for most alts then ... */
        if (left_alt)
            *p++ = '\033';

        /* Lets see if it's a pattern we know all about ... */
        if (wParam == VK_PRIOR && shift_state == 1) {
            scroll_cmd(wgs, WUI_SCROLL_PAGEUP, 0);
            return 0;
        }
        if (wParam == VK_PRIOR && shift_state == 3) { /* ctrl-shift-pageup */
            scroll_cmd(wgs, WUI_SCROLL_TOP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 3) { /* ctrl-shift-pagedown */
            scroll_cmd(wgs, WUI_SCROLL_BOTTOM, 0);
            return 0;
        }

        if (wParam == VK_PRIOR && shift_state == 2) {
            scroll_cmd(wgs, WUI_SCROLL_LINEUP, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 1) {
            scroll_cmd(wgs, WUI_SCROLL_PAGEDOWN, 0);
            return 0;
        }
        if (wParam == VK_NEXT && shift_state == 2) {
            scroll_cmd(wgs, WUI_SCROLL_LINEDOWN, 0);
            return 0;
        }
        if ((wParam == VK_PRIOR || wParam == VK_NEXT) && shift_state == 3) {
            term_scroll_to_selection(wgs->term, (wParam == VK_PRIOR ? 0 : 1));
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 2) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == VK_INSERT && shift_state == 1) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftins)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'C' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                break;          /* no need to re-copy to CLIP_LOCAL */
              case CLIPUI_EXPLICIT:
                term_request_copy(wgs->term, clips_system,
                                  lenof(clips_system));
                break;
              default:
                break;
            }
            return 0;
        }
        if (wParam == 'V' && shift_state == 3) {
            switch (conf_get_int(wgs->conf, CONF_ctrlshiftcv)) {
              case CLIPUI_IMPLICIT:
                term_request_paste(wgs->term, CLIP_LOCAL);
                break;
              case CLIPUI_EXPLICIT:
                term_request_paste(wgs->term, CLIP_SYSTEM);
                break;
              default:
                break;
            }
            return 0;
        }
        if (left_alt && wParam == VK_F4 &&
            conf_get_bool(wgs->conf, CONF_alt_f4)) {
            return -1;
        }
        if (left_alt && wParam == VK_SPACE &&
            conf_get_bool(wgs->conf, CONF_alt_space)) {
            /* (Windows sends that on as SC_KEYMENU, for which
             * TermWndProc brings up the window icon's menu) */
            show_mouseptr(wgs, true);
            return -1;
        }
        if (left_alt && wParam == VK_RETURN &&
            conf_get_bool(wgs->conf, CONF_fullscreenonaltenter) &&
            (conf_get_int(wgs->conf, CONF_resize_action) != RESIZE_DISABLED)) {
            if ((HIWORD(lParam) & (KF_UP | KF_REPEAT)) != KF_REPEAT)
                PostMessage(msg_hwnd, WM_WUI_COMMAND, IDM_FULLSCREEN, 0);
            return 0;
        }
        /* Control-Numlock for app-keypad mode switch */
        if (wParam == VK_PAUSE && shift_state == 2) {
            wgs->term->app_keypad_keys = !wgs->term->app_keypad_keys;
            return 0;
        }

        if (wParam == VK_BACK && shift_state == 0) {    /* Backspace */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x7F : 0x08);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_BACK && shift_state == 1) {    /* Shift Backspace */
            /* We do the opposite of what is configured */
            *p++ = (conf_get_bool(wgs->conf, CONF_bksp_is_delete) ?
                    0x08 : 0x7F);
            *p++ = 0;
            return -2;
        }
        if (wParam == VK_TAB && shift_state == 1) {     /* Shift tab */
            *p++ = 0x1B;
            *p++ = '[';
            *p++ = 'Z';
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 2) {   /* Ctrl-Space */
            *p++ = 0;
            return p - output;
        }
        if (wParam == VK_SPACE && shift_state == 3) {   /* Ctrl-Shift-Space */
            *p++ = 160;
            return p - output;
        }
        if (wParam == VK_CANCEL && shift_state == 2) {  /* Ctrl-Break */
            if (wgs->backend)
                backend_special(wgs->backend, SS_BRK, 0);
            return 0;
        }
        if (wParam == VK_PAUSE) {      /* Break/Pause */
            *p++ = 26;
            *p++ = 0;
            return -2;
        }
        /* Control-2 to Control-8 are special */
        if (shift_state == 2 && wParam >= '2' && wParam <= '8') {
            *p++ = "\000\033\034\035\036\037\177"[wParam - '2'];
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xBD || wParam == 0xBF)) {
            *p++ = 0x1F;
            return p - output;
        }
        if (shift_state == 2 && (wParam == 0xDF || wParam == 0xDC)) {
            *p++ = 0x1C;
            return p - output;
        }
        if (shift_state == 3 && wParam == 0xDE) {
            *p++ = 0x1E;               /* Ctrl-~ == Ctrl-^ in xterm at least */
            return p - output;
        }

        switch (wParam) {
            bool consumed_alt;

          case VK_NUMPAD0: keypad_key = '0'; goto numeric_keypad;
          case VK_NUMPAD1: keypad_key = '1'; goto numeric_keypad;
          case VK_NUMPAD2: keypad_key = '2'; goto numeric_keypad;
          case VK_NUMPAD3: keypad_key = '3'; goto numeric_keypad;
          case VK_NUMPAD4: keypad_key = '4'; goto numeric_keypad;
          case VK_NUMPAD5: keypad_key = '5'; goto numeric_keypad;
          case VK_NUMPAD6: keypad_key = '6'; goto numeric_keypad;
          case VK_NUMPAD7: keypad_key = '7'; goto numeric_keypad;
          case VK_NUMPAD8: keypad_key = '8'; goto numeric_keypad;
          case VK_NUMPAD9: keypad_key = '9'; goto numeric_keypad;
          case VK_DECIMAL: keypad_key = '.'; goto numeric_keypad;
          case VK_ADD: keypad_key = '+'; goto numeric_keypad;
          case VK_SUBTRACT: keypad_key = '-'; goto numeric_keypad;
          case VK_MULTIPLY: keypad_key = '*'; goto numeric_keypad;
          case VK_DIVIDE: keypad_key = '/'; goto numeric_keypad;
          case VK_EXECUTE: keypad_key = 'G'; goto numeric_keypad;
            /* also the case for VK_RETURN below can sometimes come here */
          numeric_keypad:
            /* Left Alt overrides all numeric keypad usage to act as
             * numeric character code input */
            if (left_alt) {
                if (keypad_key >= '0' && keypad_key <= '9')
                    wgs->alt_numberpad_accumulator =
                        wgs->alt_numberpad_accumulator * 10 + keypad_key - '0';
                else
                    wgs->alt_numberpad_accumulator = 0;
                break;
            }

            {
                int nchars = format_numeric_keypad_key(
                    (char *)p, wgs->term, keypad_key,
                    shift_state & 1, shift_state & 2);
                if (!nchars) {
                    /*
                     * If we didn't get an escape sequence out of the
                     * numeric keypad key, then that must be because
                     * we're in Num Lock mode without application
                     * keypad enabled. In that situation we leave this
                     * keypress to the ToUnicode/ToAsciiEx handler
                     * below, which will translate it according to the
                     * appropriate keypad layout (e.g. so that what a
                     * Brit thinks of as keypad '.' can become ',' in
                     * the German layout).
                     *
                     * An exception is the keypad Return key: if we
                     * didn't get an escape sequence for that, we
                     * treat it like ordinary Return, taking into
                     * account Telnet special new line codes and
                     * config options.
                     */
                    if (keypad_key == '\r')
                        goto ordinary_return_key;
                    break;
                }

                p += nchars;
                return p - output;
            }

            int fkey_number;
          case VK_F1: fkey_number = 1; goto numbered_function_key;
          case VK_F2: fkey_number = 2; goto numbered_function_key;
          case VK_F3: fkey_number = 3; goto numbered_function_key;
          case VK_F4: fkey_number = 4; goto numbered_function_key;
          case VK_F5: fkey_number = 5; goto numbered_function_key;
          case VK_F6: fkey_number = 6; goto numbered_function_key;
          case VK_F7: fkey_number = 7; goto numbered_function_key;
          case VK_F8: fkey_number = 8; goto numbered_function_key;
          case VK_F9: fkey_number = 9; goto numbered_function_key;
          case VK_F10: fkey_number = 10; goto numbered_function_key;
          case VK_F11: fkey_number = 11; goto numbered_function_key;
          case VK_F12: fkey_number = 12; goto numbered_function_key;
          case VK_F13: fkey_number = 13; goto numbered_function_key;
          case VK_F14: fkey_number = 14; goto numbered_function_key;
          case VK_F15: fkey_number = 15; goto numbered_function_key;
          case VK_F16: fkey_number = 16; goto numbered_function_key;
          case VK_F17: fkey_number = 17; goto numbered_function_key;
          case VK_F18: fkey_number = 18; goto numbered_function_key;
          case VK_F19: fkey_number = 19; goto numbered_function_key;
          case VK_F20: fkey_number = 20; goto numbered_function_key;
          numbered_function_key:
            consumed_alt = false;
            p += format_function_key((char *)p, wgs->term, fkey_number,
                                     shift_state & 1, shift_state & 2,
                                     left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            SmallKeypadKey sk_key;
          case VK_HOME: sk_key = SKK_HOME; goto small_keypad_key;
          case VK_END: sk_key = SKK_END; goto small_keypad_key;
          case VK_INSERT: sk_key = SKK_INSERT; goto small_keypad_key;
          case VK_DELETE: sk_key = SKK_DELETE; goto small_keypad_key;
          case VK_PRIOR: sk_key = SKK_PGUP; goto small_keypad_key;
          case VK_NEXT: sk_key = SKK_PGDN; goto small_keypad_key;
          small_keypad_key:
            /* These keys don't generate terminal input with Ctrl */
            if (shift_state & 2)
                break;

            consumed_alt = false;
            p += format_small_keypad_key((char *)p, wgs->term, sk_key,
                                         shift_state & 1, shift_state & 2,
                                         left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

            char xkey;
          case VK_UP: xkey = 'A'; goto arrow_key;
          case VK_DOWN: xkey = 'B'; goto arrow_key;
          case VK_RIGHT: xkey = 'C'; goto arrow_key;
          case VK_LEFT: xkey = 'D'; goto arrow_key;
          case VK_CLEAR: xkey = 'G'; goto arrow_key; /* close enough */
          arrow_key:
            consumed_alt = false;
            p += format_arrow_key((char *)p, wgs->term, xkey, shift_state & 1,
                                  shift_state & 2, left_alt, &consumed_alt);
            if (consumed_alt) {
                /* supersedes the usual prefixing of Esc */
                p -= 1;
                memmove(output, output + 1, p - output);
            }
            return p - output;

          case VK_RETURN:
            if (HIWORD(lParam) & KF_EXTENDED) {
                keypad_key = '\r';
                goto numeric_keypad;
            }
          ordinary_return_key:
            if (shift_state == 0 && wgs->term->cr_lf_return) {
                *p++ = '\r';
                *p++ = '\n';
                return p - output;
            } else {
                *p++ = 0x0D;
                *p++ = 0;
                return -2;
            }
        }
    }

    /* Okay we've done everything interesting; let windows deal with
     * the boring stuff */
    {
        bool capsOn = false;

        /* helg: clear CAPS LOCK state if caps lock switches to cyrillic */
        if (keystate[VK_CAPITAL] != 0 &&
            conf_get_bool(wgs->conf, CONF_xlat_capslockcyr)) {
            capsOn = !left_alt;
            keystate[VK_CAPITAL] = 0;
        }

        wchar_t keys_unicode[3];

        /* XXX how do we know what the max size of the keys array should
         * be is? There's indication on MS' website of an Inquire/InquireEx
         * functioning returning a KBINFO structure which tells us. */
        if (p_ToUnicodeEx) {
            r = p_ToUnicodeEx(wParam, scan, keystate, keys_unicode,
                              lenof(keys_unicode), 0, kbd_layout);
        } else {
            r = ToUnicode(wParam, scan, keystate, keys_unicode,
                          lenof(keys_unicode), 0);
        }
        if (r > 0) {
            WCHAR keybuf;

            p = output;
            for (i = 0; i < r; i++) {
                wchar_t wch = keys_unicode[i];

                if (wgs->compose_state == 2 && wch >= ' ' && wch < 0x80) {
                    wgs->compose_char = wch;
                    wgs->compose_state++;
                    continue;
                }
                if (wgs->compose_state == 3 && wch >= ' ' && wch < 0x80) {
                    int nc;
                    wgs->compose_state = 0;

                    if ((nc = check_compose(wgs->compose_char, wch)) == -1) {
                        MessageBeep(MB_ICONHAND);
                        return 0;
                    }
                    keybuf = nc;
                    term_keyinputw(wgs->term, &keybuf, 1);
                    continue;
                }

                wgs->compose_state = 0;

                if (!key_down) {
                    if (wgs->alt_numberpad_accumulator) {
                        if (in_utf(wgs->term) ||
                            wgs->ucsdata.dbcs_screenfont) {
                            keybuf = wgs->alt_numberpad_accumulator;
                            term_keyinputw(wgs->term, &keybuf, 1);
                        } else {
                            char ch = (char) wgs->alt_numberpad_accumulator;
                            /*
                             * We need not bother about stdin
                             * backlogs here, because in GUI PuTTY
                             * we can't do anything about it
                             * anyway; there's no means of asking
                             * Windows to hold off on KEYDOWN
                             * messages. We _have_ to buffer
                             * everything we're sent.
                             */
                            term_keyinput(wgs->term, -1, &ch, 1);
                        }
                        wgs->alt_numberpad_accumulator = 0;
                    } else {
                        term_keyinputw(wgs->term, &wch, 1);
                    }
                } else {
                    if (capsOn && wch < 0x80) {
                        WCHAR cbuf[2];
                        cbuf[0] = 27;
                        cbuf[1] = xlat_uskbd2cyrllic(wch);
                        term_keyinputw(
                            wgs->term, cbuf+!left_alt, 1+!!left_alt);
                    } else {
                        WCHAR cbuf[2];
                        cbuf[0] = '\033';
                        cbuf[1] = wch;
                        term_keyinputw(
                            wgs->term, cbuf +!left_alt, 1+!!left_alt);
                    }
                }
                show_mouseptr(wgs, false);
            }

            return p - output;
        }
    }

    /*
     * ALT alone may or may not want to bring up the System menu.
     * If it's not meant to, we return 0 on presses or releases of
     * ALT, to show that we've swallowed the keystroke. Otherwise
     * we return -1, which means Windows will give the keystroke
     * its default handling (i.e. bring up the System menu).
     */
    if (wParam == VK_MENU && !conf_get_bool(wgs->conf, CONF_alt_only))
        return 0;

    /*
     * Key releases, dead keys and the like are things we've dealt
     * with (or deliberately ignored), and if we passed them on, the
     * XAML framework would only translate them into characters that
     * we'd then type twice. So the only thing left for Windows is a
     * keystroke that it may want as a shortcut, i.e. one involving
     * Alt (or F10, which activates the menu).
     */
    if (left_alt || wParam == VK_MENU || wParam == VK_F10)
        return -1;
    return 0;
}

/*
 * What a session's tab says: a title of its own if it has one (set in
 * its settings, or by the server: bash's "user@machine: ~", say), or
 * else the name of the saved session it's from (just its own name, not
 * its folder's), or else the title PuTTY makes up ("host - PuTTY", or
 * "PuTTY (inactive)"). The window title, and the saved session's full
 * name, go in the tab's tooltip.
 */
static bool made_up_title(WinGuiSeat *wgs)
{
    const wchar_t *t = wgs->window_name;
    if (!t || *conf_get_str(wgs->conf, CONF_wintitle))
        return false;
    if (!*t)
        return true;                   /* (no title at all) */
    wchar_t *app = dup_mb_to_wc(DEFAULT_CODEPAGE, appname);
    wchar_t *suffix = dupwcscat(L" - ", app);
    wchar_t *inactive = dupwcscat(app, L" (inactive)");
    size_t tl = wcslen(t), sl = wcslen(suffix);
    bool made = !wcscmp(t, app) || !wcscmp(t, inactive) ||
        (tl > sl && !wcscmp(t + tl - sl, suffix));
    sfree(inactive);
    sfree(suffix);
    sfree(app);
    return made;
}

static void update_tab_label(WuiSeat *ws)
{
    if (!ws->tterm)
        return;
    wchar_t *session = ws->session_name ?
        dup_mb_to_wc(DEFAULT_CODEPAGE, ws->session_name) : NULL;
    const wchar_t *leaf = session ? wcsrchr(session, FOLDER_SEP_W) : NULL;
    leaf = leaf ? leaf + 1 : session;
    /* (and in the tooltip, with '/' between its folders: see name_shown;
     * the leaf, being after the last of them, has none) */
    if (session)
        for (wchar_t *q = session; *q; q++)
            if (*q == FOLDER_SEP_W)
                *q = L'/';
    wui_term_set_label(ws->tterm,
                       leaf && made_up_title(&ws->wgs) ? leaf : NULL,
                       session);
    sfree(session);
}

/* Give a session the name of the saved session it's from */
static void set_session_name(WuiSeat *ws, char *name)
{
    sfree(ws->session_name);
    ws->session_name = name;
    update_tab_label(ws);
}

static void wintw_set_title(TermWin *tw, const char *title, int codepage)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wchar_t *new_window_name = dup_mb_to_wc(codepage, title);
    if (!wcscmp(new_window_name, wgs->window_name)) {
        sfree(new_window_name);
        return;
    }
    sfree(wgs->window_name);
    wgs->window_name = new_window_name;
    wui_term_set_title(WUI(wgs)->tterm, wgs->window_name);
    update_tab_label(WUI(wgs));
    if (WUI(wgs) == active)
        update_window_title();
}

static void wintw_set_icon_title(TermWin *tw, const char *title, int codepage)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    wchar_t *new_icon_name = dup_mb_to_wc(codepage, title);
    if (!wcscmp(new_icon_name, wgs->icon_name)) {
        sfree(new_icon_name);
        return;
    }
    sfree(wgs->icon_name);
    wgs->icon_name = new_icon_name;
    if (WUI(wgs) == active)
        update_window_title();
}

static void update_scrollbar_visibility(WinGuiSeat *wgs)
{
    wui_set_scrollbar_visible(
        WUI(wgs)->tterm, conf_get_bool(wgs->conf, is_full_screen(wgs) ?
                                      CONF_scrollbar_in_fullscreen :
                                      CONF_scrollbar));
}

static void wintw_set_scrollbar(TermWin *tw, int total, int start, int page)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);

    if (!conf_get_bool(wgs->conf, is_full_screen(wgs) ?
                       CONF_scrollbar_in_fullscreen : CONF_scrollbar))
        return;

    if (WUI(wgs)->tterm)
        wui_set_scrollbar(WUI(wgs)->tterm, total, start, page);
}

static bool wintw_setup_draw_ctx(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    WuiSeat *ws = WUI(wgs);
    assert(!ws->drawing);
    if (!ws->tterm || !wui_begin_draw(ws->tterm))
        return false;
    ws->drawing = true;
    return true;
}

static void wintw_free_draw_ctx(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    WuiSeat *ws = WUI(wgs);
    assert(ws->drawing);

    if (ws->border_dirty) {
        /* Fill in the space between the text area and the window
         * edges with the default background colour. */
        int pw, ph;
        int tw_ = wgs->font_width * wgs->term->cols;
        int th = wgs->font_height * wgs->term->rows;
        COLORREF bg = wgs->colours[ATTR_DEFBG>>ATTR_BGSHIFT];
        int ox = wgs->offset_width, oy = wgs->offset_height;

        wui_get_panel_size(ws->tterm, &pw, &ph);
        if (oy > 0)
            fill_rect(wgs, 0, 0, pw, oy, bg);
        if (oy + th < ph)
            fill_rect(wgs, 0, oy + th, pw, ph - (oy + th), bg);
        if (ox > 0)
            fill_rect(wgs, 0, oy, ox, th, bg);
        if (ox + tw_ < pw)
            fill_rect(wgs, ox + tw_, oy, pw - (ox + tw_), th, bg);
        ws->border_dirty = false;
    }

    wui_end_draw(ws->tterm);
    ws->drawing = false;
}

/*
 * Set up the colour palette. (No real palettes any more: we just
 * keep the RGB values, which the RTF clipboard code wants.)
 */
static void init_palette(WinGuiSeat *wgs)
{
    wgs->pal = NULL;
    wgs->logpal = snew_plus(
        LOGPALETTE, (OSC4_NCOLOURS - 1) * sizeof(PALETTEENTRY));
    wgs->logpal->palVersion = 0x300;
    wgs->logpal->palNumEntries = OSC4_NCOLOURS;
    for (unsigned i = 0; i < OSC4_NCOLOURS; i++)
        wgs->logpal->palPalEntry[i].peFlags = PC_NOCOLLAPSE;
}

static void wintw_palette_set(TermWin *tw, unsigned start,
                              unsigned ncolours, const rgb *colours_in)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(start <= OSC4_NCOLOURS);
    assert(ncolours <= OSC4_NCOLOURS - start);

    for (unsigned i = 0; i < ncolours; i++) {
        const rgb *in = &colours_in[i];
        PALETTEENTRY *out = &wgs->logpal->palPalEntry[i + start];
        out->peRed = in->r;
        out->peGreen = in->g;
        out->peBlue = in->b;
        wgs->colours[i + start] = RGB(in->r, in->g, in->b);
    }

    if (start <= OSC4_COLOUR_bg && OSC4_COLOUR_bg < start + ncolours) {
        /* If Default Background changes, we need to ensure any space between
         * the text area and the window border is redrawn. */
        invalidate_all(wgs);
        wui_term_set_background(WUI(wgs)->tterm,
                                wgs->colours[ATTR_DEFBG>>ATTR_BGSHIFT]);
    }
}

/* ----------------------------------------------------------------------
 * Clipboard. Our hidden message window is the clipboard owner.
 */

void write_aclip(HWND hwnd, int clipboard, char *data, int len)
{
    HGLOBAL clipdata;
    void *lock;

    if (clipboard != CLIP_SYSTEM)
        return;

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len + 1);
    if (!clipdata)
        return;
    lock = GlobalLock(clipdata);
    if (!lock)
        return;
    memcpy(lock, data, len);
    ((unsigned char *) lock)[len] = 0;
    GlobalUnlock(clipdata);

    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_TEXT, clipdata);
        CloseClipboard();
    } else
        GlobalFree(clipdata);
}

typedef struct _rgbindex {
    int index;
    COLORREF ref;
} rgbindex;

static int cmpCOLORREF(void *va, void *vb)
{
    COLORREF a = ((rgbindex *)va)->ref;
    COLORREF b = ((rgbindex *)vb)->ref;
    return (a < b) ? -1 : (a > b) ? +1 : 0;
}

/*
 * Note: unlike write_aclip() this will not append a nul.
 */
static void wintw_clip_write(
    TermWin *tw, int clipboard, wchar_t *data, int *attr,
    truecolour *truecolour, int len, bool must_deselect)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    HGLOBAL clipdata, clipdata2, clipdata3;
    int len2;
    void *lock, *lock2, *lock3;

    if (clipboard != CLIP_SYSTEM)
        return;

    /* (into this tab's pool too, and where Copy to Pool said) */
    pool_copied(WUI(wgs), data, len);

    len2 = WideCharToMultiByte(CP_ACP, 0, data, len, 0, 0, NULL, NULL);

    clipdata = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE,
                           len * sizeof(wchar_t));
    clipdata2 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, len2);

    if (!clipdata || !clipdata2) {
        if (clipdata)
            GlobalFree(clipdata);
        if (clipdata2)
            GlobalFree(clipdata2);
        return;
    }
    if (!(lock = GlobalLock(clipdata))) {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }
    if (!(lock2 = GlobalLock(clipdata2))) {
        GlobalUnlock(clipdata);
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
        return;
    }

    memcpy(lock, data, len * sizeof(wchar_t));
    WideCharToMultiByte(CP_ACP, 0, data, len, lock2, len2, NULL, NULL);

    if (conf_get_bool(wgs->conf, CONF_rtf_paste)) {
        wchar_t unitab[256];
        strbuf *rtf = strbuf_new();
        unsigned char *tdata = (unsigned char *)lock2;
        wchar_t *udata = (wchar_t *)lock;
        int uindex = 0, tindex = 0;
        int multilen, blen, alen, i;
        char before[16], after[4];
        int fgcolour,  lastfgcolour  = -1;
        int bgcolour,  lastbgcolour  = -1;
        COLORREF fg,   lastfg = -1;
        COLORREF bg,   lastbg = -1;
        int attrBold,  lastAttrBold  = 0;
        int attrUnder, lastAttrUnder = 0;
        int palette[OSC4_NCOLOURS];
        int numcolours;
        tree234 *rgbtree = NULL;
        FontSpec *font = conf_get_fontspec(wgs->conf, CONF_font);

        get_unitab(CP_ACP, unitab, 0);

        put_fmt(
            rtf, "{\\rtf1\\ansi\\deff0{\\fonttbl\\f0\\fmodern %s;}\\f0\\fs%d",
            font->name, font->height*2);

        /*
         * Add colour palette
         * {\colortbl ;\red255\green0\blue0;\red0\green0\blue128;}
         */

        /*
         * First - Determine all colours in use
         *    o  Foregound and background colours share the same palette
         */
        if (attr) {
            memset(palette, 0, sizeof(palette));
            for (i = 0; i < (len-1); i++) {
                fgcolour = ((attr[i] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                bgcolour = ((attr[i] & ATTR_BGMASK) >> ATTR_BGSHIFT);

                if (attr[i] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;   /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;
                }

                if (wgs->bold_colours && (attr[i] & ATTR_BOLD)) {
                    if (fgcolour  <   8)        /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)   /* Default colours */
                        fgcolour ++;
                }

                if ((attr[i] & ATTR_BLINK)) {
                    if (bgcolour  <   8)        /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)   /* Default colours */
                        bgcolour ++;
                }

                palette[fgcolour]++;
                palette[bgcolour]++;
            }

            if (truecolour) {
                rgbtree = newtree234(cmpCOLORREF);
                for (i = 0; i < (len-1); i++) {
                    if (truecolour[i].fg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].fg.r,
                                        truecolour[i].fg.g,
                                        truecolour[i].fg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                    if (truecolour[i].bg.enabled) {
                        rgbindex *rgbp = snew(rgbindex);
                        rgbp->ref = RGB(truecolour[i].bg.r,
                                        truecolour[i].bg.g,
                                        truecolour[i].bg.b);
                        if (add234(rgbtree, rgbp) != rgbp)
                            sfree(rgbp);
                    }
                }
            }

            /*
             * Next - Create a reduced palette
             */
            numcolours = 0;
            for (i = 0; i < OSC4_NCOLOURS; i++) {
                if (palette[i] != 0)
                    palette[i]  = ++numcolours;
            }

            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    rgbp->index = ++numcolours;
            }

            /*
             * Finally - Write the colour table
             */
            put_datapl(rtf, PTRLEN_LITERAL("{\\colortbl ;"));

            for (i = 0; i < OSC4_NCOLOURS; i++) {
                if (palette[i] != 0) {
                    const PALETTEENTRY *pe = &wgs->logpal->palPalEntry[i];
                    put_fmt(rtf, "\\red%d\\green%d\\blue%d;",
                            pe->peRed, pe->peGreen, pe->peBlue);
                }
            }
            if (rgbtree) {
                rgbindex *rgbp;
                for (i = 0; (rgbp = index234(rgbtree, i)) != NULL; i++)
                    put_fmt(rtf, "\\red%d\\green%d\\blue%d;",
                            GetRValue(rgbp->ref), GetGValue(rgbp->ref),
                            GetBValue(rgbp->ref));
            }
            put_datapl(rtf, PTRLEN_LITERAL("}"));
        }

        /*
         * We want to construct a piece of RTF that specifies the
         * same Unicode text. To do this we will read back in
         * parallel from the Unicode data in `udata' and the
         * non-Unicode data in `tdata'. For each character in
         * `tdata' which becomes the right thing in `udata' when
         * looked up in `unitab', we just copy straight over from
         * tdata. For each one that doesn't, we must WCToMB it
         * individually and produce a \u escape sequence.
         */
        while (tindex < len2 && uindex < len &&
               tdata[tindex] && udata[uindex]) {
            if (tindex + 1 < len2 &&
                tdata[tindex] == '\r' &&
                tdata[tindex+1] == '\n') {
                tindex++;
                uindex++;
            }

            /*
             * Set text attributes
             */
            if (attr) {
                /*
                 * Determine foreground and background colours
                 */
                if (truecolour && truecolour[tindex].fg.enabled) {
                    fgcolour = -1;
                    fg = RGB(truecolour[tindex].fg.r,
                             truecolour[tindex].fg.g,
                             truecolour[tindex].fg.b);
                } else {
                    fgcolour = ((attr[tindex] & ATTR_FGMASK) >> ATTR_FGSHIFT);
                    fg = -1;
                }

                if (truecolour && truecolour[tindex].bg.enabled) {
                    bgcolour = -1;
                    bg = RGB(truecolour[tindex].bg.r,
                             truecolour[tindex].bg.g,
                             truecolour[tindex].bg.b);
                } else {
                    bgcolour = ((attr[tindex] & ATTR_BGMASK) >> ATTR_BGSHIFT);
                    bg = -1;
                }

                if (attr[tindex] & ATTR_REVERSE) {
                    int tmpcolour = fgcolour;       /* Swap foreground and background */
                    fgcolour = bgcolour;
                    bgcolour = tmpcolour;

                    COLORREF tmpref = fg;
                    fg = bg;
                    bg = tmpref;
                }

                if (wgs->bold_colours && (attr[tindex] & ATTR_BOLD) &&
                    (fgcolour >= 0)) {
                    if (fgcolour  <   8)            /* ANSI colours */
                        fgcolour +=   8;
                    else if (fgcolour >= 256)       /* Default colours */
                        fgcolour ++;
                }

                if ((attr[tindex] & ATTR_BLINK) && (bgcolour >= 0)) {
                    if (bgcolour  <   8)            /* ANSI colours */
                        bgcolour +=   8;
                    else if (bgcolour >= 256)       /* Default colours */
                        bgcolour ++;
                }

                /*
                 * Collect other attributes
                 */
                if (wgs->bold_font_mode != BOLD_NONE)
                    attrBold  = attr[tindex] & ATTR_BOLD;
                else
                    attrBold  = 0;

                attrUnder = attr[tindex] & ATTR_UNDER;

                /*
                 * Reverse video
                 *   o  If video isn't reversed, ignore colour attributes for default foregound
                 *      or background.
                 *   o  Special case where bolded text is displayed using the default foregound
                 *      and background colours - force to bolded RTF.
                 */
                if (!(attr[tindex] & ATTR_REVERSE)) {
                    if (bgcolour >= 256)            /* Default color */
                        bgcolour  = -1;             /* No coloring */

                    if (fgcolour >= 256) {          /* Default colour */
                        if (wgs->bold_colours && (fgcolour & 1) &&
                            bgcolour == -1)
                            attrBold = ATTR_BOLD;   /* Emphasize text with bold attribute */

                        fgcolour  = -1;             /* No coloring */
                    }
                }

                /*
                 * Write RTF text attributes
                 */
                if ((lastfgcolour != fgcolour) || (lastfg != fg)) {
                    lastfgcolour  = fgcolour;
                    lastfg        = fg;
                    if (fg == -1) {
                        put_fmt(rtf, "\\cf%d ",
                                (fgcolour >= 0) ? palette[fgcolour] : 0);
                    } else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = fg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            put_fmt(rtf, "\\cf%d ", rgbp->index);
                    }
                }

                if ((lastbgcolour != bgcolour) || (lastbg != bg)) {
                    lastbgcolour  = bgcolour;
                    lastbg        = bg;
                    if (bg == -1)
                        put_fmt(rtf, "\\highlight%d ",
                                (bgcolour >= 0) ? palette[bgcolour] : 0);
                    else {
                        rgbindex rgb, *rgbp;
                        rgb.ref = bg;
                        if ((rgbp = find234(rgbtree, &rgb, NULL)) != NULL)
                            put_fmt(rtf, "\\highlight%d ", rgbp->index);
                    }
                }

                if (lastAttrBold != attrBold) {
                    lastAttrBold  = attrBold;
                    put_datapl(rtf, attrBold ?
                               PTRLEN_LITERAL("\\b ") :
                               PTRLEN_LITERAL("\\b0 "));
                }

                if (lastAttrUnder != attrUnder) {
                    lastAttrUnder  = attrUnder;
                    put_datapl(rtf, attrUnder ?
                               PTRLEN_LITERAL("\\ul ") :
                               PTRLEN_LITERAL("\\ulnone "));
                }
            }

            if (unitab[tdata[tindex]] == udata[uindex]) {
                multilen = 1;
                before[0] = '\0';
                after[0] = '\0';
                blen = alen = 0;
            } else {
                multilen = WideCharToMultiByte(CP_ACP, 0, unitab+uindex, 1,
                                               NULL, 0, NULL, NULL);
                if (multilen != 1) {
                    blen = sprintf(before, "{\\uc%d\\u%d", (int)multilen,
                                   (int)udata[uindex]);
                    alen = 1; strcpy(after, "}");
                } else {
                    blen = sprintf(before, "\\u%d", (int)udata[uindex]);
                    alen = 0; after[0] = '\0';
                }
            }
            assert(tindex + multilen <= len2);

            put_data(rtf, before, blen);
            for (i = 0; i < multilen; i++) {
                if (tdata[tindex+i] == '\\' ||
                    tdata[tindex+i] == '{' ||
                    tdata[tindex+i] == '}') {
                    put_byte(rtf, '\\');
                    put_byte(rtf, tdata[tindex+i]);
                } else if (tdata[tindex+i] == 0x0D || tdata[tindex+i] == 0x0A) {
                    put_datapl(rtf, PTRLEN_LITERAL("\\par\r\n"));
                } else if (tdata[tindex+i] > 0x7E || tdata[tindex+i] < 0x20) {
                    put_fmt(rtf, "\\'%02x", tdata[tindex+i]);
                } else {
                    put_byte(rtf, tdata[tindex+i]);
                }
            }
            put_data(rtf, after, alen);

            tindex += multilen;
            uindex++;
        }

        put_datapl(rtf, PTRLEN_LITERAL("}\0\0")); /* Terminate RTF stream */

        clipdata3 = GlobalAlloc(GMEM_DDESHARE | GMEM_MOVEABLE, rtf->len);
        if (clipdata3 && (lock3 = GlobalLock(clipdata3)) != NULL) {
            memcpy(lock3, rtf->u, rtf->len);
            GlobalUnlock(clipdata3);
        }
        strbuf_free(rtf);

        if (rgbtree) {
            rgbindex *rgbp;
            while ((rgbp = delpos234(rgbtree, 0)) != NULL)
                sfree(rgbp);
            freetree234(rgbtree);
        }
    } else
        clipdata3 = NULL;

    GlobalUnlock(clipdata);
    GlobalUnlock(clipdata2);

    if (!must_deselect)
        SendMessage(msg_hwnd, WM_IGNORE_CLIP, true, 0);

    clip_owner = WUI(wgs)->id;
    if (OpenClipboard(msg_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, clipdata);
        SetClipboardData(CF_TEXT, clipdata2);
        if (clipdata3)
            SetClipboardData(RegisterClipboardFormat(CF_RTF), clipdata3);
        CloseClipboard();
    } else {
        GlobalFree(clipdata);
        GlobalFree(clipdata2);
    }

    if (!must_deselect)
        SendMessage(msg_hwnd, WM_IGNORE_CLIP, false, 0);
}

static DWORD WINAPI clipboard_read_threadfunc(void *param)
{
    /* param is the id of the session that wants the paste */
    struct clipdata_msg cm;
    HGLOBAL clipdata;

    cm.seat_id = (unsigned)(uintptr_t)param;
    if (OpenClipboard(NULL)) {
        if ((clipdata = GetClipboardData(CF_UNICODETEXT))) {
            cm.unicode = true;
            SendMessage(msg_hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)&cm, (LPARAM)clipdata);
        } else if ((clipdata = GetClipboardData(CF_TEXT))) {
            cm.unicode = false;
            SendMessage(msg_hwnd, WM_GOT_CLIPDATA,
                        (WPARAM)&cm, (LPARAM)clipdata);
        }
        CloseClipboard();
    }

    return 0;
}

static void process_clipdata(WinGuiSeat *wgs, HGLOBAL clipdata, bool unicode)
{
    wchar_t *clipboard_contents = NULL;
    size_t clipboard_length = 0;

    if (unicode) {
        wchar_t *p = GlobalLock(clipdata);
        wchar_t *p2;

        if (p) {
            /* Unwilling to rely on Windows having wcslen() */
            for (p2 = p; *p2; p2++);
            clipboard_length = p2 - p;
            clipboard_contents = snewn(clipboard_length + 1, wchar_t);
            memcpy(clipboard_contents, p, clipboard_length * sizeof(wchar_t));
            clipboard_contents[clipboard_length] = L'\0';
            term_do_paste(wgs->term, clipboard_contents, clipboard_length);
        }
    } else {
        char *s = GlobalLock(clipdata);
        int i;

        if (s) {
            i = MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1, 0, 0);
            clipboard_contents = snewn(i, wchar_t);
            MultiByteToWideChar(CP_ACP, 0, s, strlen(s) + 1,
                                clipboard_contents, i);
            clipboard_length = i - 1;
            clipboard_contents[clipboard_length] = L'\0';
            term_do_paste(wgs->term, clipboard_contents, clipboard_length);
        }
    }

    sfree(clipboard_contents);
}

static void wintw_clip_request_paste(TermWin *tw, int clipboard)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    assert(clipboard == CLIP_SYSTEM);

    /*
     * The clipboard-reading functions may block, e.g. if the
     * application delivering the clipboard data has to get hold of
     * it over a network connection forwarded through this very
     * PuTTY. Hence, we spawn a subthread to read the clipboard, and
     * do our paste when it's finished. The thread will send a
     * message back to our message window when it has the data.
     */
    DWORD in_threadid; /* required for Win9x */
    HANDLE hThread = CreateThread(NULL, 0, clipboard_read_threadfunc,
                                  (void *)(uintptr_t)WUI(wgs)->id,
                                  0, &in_threadid);
    if (hThread)
        CloseHandle(hThread);          /* we don't need the thread handle */
}

/*
 * Print a modal (Really Bad) message box and perform a fatal exit.
 */
void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    show_mouseptr(NULL, true);
    title = dupprintf("%s Fatal Error", appname);
    MessageBox(find_window_for_msgbox(), message, title,
               MB_SYSTEMMODAL | MB_ICONERROR | MB_OK);
    sfree(message);
    sfree(title);
    cleanup_exit(1);
}

/*
 * Print a message box and don't close the connection.
 */
void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *message, *title;

    va_start(ap, fmt);
    message = dupvprintf(fmt, ap);
    va_end(ap);
    show_mouseptr(NULL, true);
    title = dupprintf("%s Error", appname);
    show_message(title, message, NULL, true);
    sfree(message);
    sfree(title);
}

static bool flash_window_ex(WinGuiSeat *wgs, DWORD dwFlags,
                            UINT uCount, DWORD dwTimeout)
{
    if (p_FlashWindowEx) {
        FLASHWINFO fi;
        fi.cbSize = sizeof(fi);
        fi.hwnd = wgs->term_hwnd;
        fi.dwFlags = dwFlags;
        fi.uCount = uCount;
        fi.dwTimeout = dwTimeout;
        return (*p_FlashWindowEx)(&fi);
    }
    else
        return false; /* shrug */
}

/*
 * Manage window caption / taskbar flashing, if enabled.
 * 0 = stop, 1 = maintain, 2 = start
 */
static void flash_window(WinGuiSeat *wgs, int mode)
{
    int beep_ind = conf_get_int(wgs->conf, CONF_beep_ind);
    if ((mode == 0) || (beep_ind == B_IND_DISABLED)) {
        /* stop */
        if (wgs->flashing) {
            wgs->flashing = false;
            flash_window_ex(wgs, FLASHW_STOP, 0, 0);
        }

    } else if (mode == 2) {
        /* start */
        if (!wgs->flashing) {
            wgs->flashing = true;
            /* For so-called "steady" mode, we use uCount=2, which
             * seems to be the traditional number of flashes used
             * by user notifications (e.g., by Explorer).
             * uCount=0 appears to enable continuous flashing, per
             * "flashing" mode, although I haven't seen this
             * documented. */
            flash_window_ex(wgs, FLASHW_ALL | FLASHW_TIMER,
                            (beep_ind == B_IND_FLASH ? 0 : 2),
                            0 /* system cursor blink rate */);
        }
    }
}

/*
 * Beep.
 */
static void wintw_bell(TermWin *tw, int mode)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (mode == BELL_DEFAULT) {
        /*
         * For MessageBeep style bells, we want to be careful of
         * timing, because they don't have the nice property of
         * PlaySound bells that each one cancels the previous
         * active one. So we limit the rate to one per 50ms or so.
         */
        long beepdiff;

        beepdiff = GetTickCount() - wgs->last_beep_time;
        if (beepdiff >= 0 && beepdiff < 50)
            return;
        MessageBeep(MB_OK);
        /*
         * The above MessageBeep call takes time, so we record the
         * time _after_ it finishes rather than before it starts.
         */
        wgs->last_beep_time = GetTickCount();
    } else if (mode == BELL_WAVEFILE) {
        Filename *bell_wavefile = conf_get_filename(
            wgs->conf, CONF_bell_wavefile);
        bool success = (
            p_PlaySoundW ? p_PlaySoundW(bell_wavefile->wpath, NULL,
                                        SND_ASYNC | SND_FILENAME) :
            p_PlaySoundA ? p_PlaySoundA(bell_wavefile->cpath, NULL,
                                        SND_ASYNC | SND_FILENAME) : false);
        if (!success) {
            char *buf, *otherbuf;
            show_mouseptr(wgs, true);
            buf = dupprintf(
                "Unable to play sound file\n%s\nUsing default sound instead",
                bell_wavefile->utf8path);
            otherbuf = dupprintf("%s Sound Error", appname);
            show_message(otherbuf, buf, NULL, true);
            sfree(buf);
            sfree(otherbuf);
            conf_set_int(wgs->conf, CONF_beep, BELL_DEFAULT);
        }
    } else if (mode == BELL_PCSPEAKER) {
        long beepdiff;

        beepdiff = GetTickCount() - wgs->last_beep_time;
        if (beepdiff >= 0 && beepdiff < 50)
            return;

        Beep(800, 100);
        wgs->last_beep_time = GetTickCount();
    }
    /* Otherwise, either visual bell or disabled; do nothing here */
    if (!wgs->term->has_focus) {
        flash_window(wgs, 2);               /* start */
    }
}

/*
 * Minimise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_minimised(TermWin *tw, bool minimised)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    WuiWindowState state = wui_get_state(host);
    if (WUI(wgs) != active)
        return;        /* sessions in hidden tabs don't get a say */
    if (state == WUI_STATE_MINIMISED) {
        if (!minimised)
            wui_set_state(host, WUI_STATE_NORMAL);
    } else {
        if (minimised)
            wui_set_state(host, WUI_STATE_MINIMISED);
    }
}

/*
 * Move the window in response to a server-side request.
 */
static void wintw_move(TermWin *tw, int x, int y)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    int resize_action = conf_get_int(wgs->conf, CONF_resize_action);
    if (resize_action == RESIZE_DISABLED ||
        resize_action == RESIZE_FONT ||
        is_zoomed(wgs) || WUI(wgs) != active)
        return;

    SetWindowPos(wgs->term_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

/*
 * Move the window to the top or bottom of the z-order in response
 * to a server-side request.
 */
static void wintw_set_zorder(TermWin *tw, bool top)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (conf_get_bool(wgs->conf, CONF_alwaysontop) || WUI(wgs) != active)
        return;                        /* ignore */
    SetWindowPos(wgs->term_hwnd, top ? HWND_TOP : HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

/*
 * Refresh the window in response to a server-side request.
 */
static void wintw_refresh(TermWin *tw)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    invalidate_all(wgs);
}

/*
 * Maximise or restore the window in response to a server-side
 * request.
 */
static void wintw_set_maximised(TermWin *tw, bool maximised)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (WUI(wgs) != active)
        return;
    if (win_state == WUI_STATE_MAXIMISED ||
        win_state == WUI_STATE_FULLSCREEN) {
        if (!maximised)
            wui_set_state(host, WUI_STATE_NORMAL);
    } else {
        if (maximised)
            wui_set_state(host, WUI_STATE_MAXIMISED);
    }
}

/*
 * See if we're in full-screen mode.
 */
static bool is_full_screen(WinGuiSeat *wgs)
{
    return host && wui_get_state(host) == WUI_STATE_FULLSCREEN;
}

/*
 * See if the window size is out of this session's hands: if it's
 * maximised or full-screen, or if it's shared with other tabs. In all
 * of these cases the session has to fit itself into the terminal area
 * it's given, as window.c does for a maximised window.
 */
static bool is_zoomed(WinGuiSeat *wgs)
{
    WuiWindowState state = wui_get_state(host);
    return state == WUI_STATE_MAXIMISED || state == WUI_STATE_FULLSCREEN ||
        nseats > 1;
}

/* Get a MONITORINFO structure for the nearest available monitor, if the
 * multimon API is available and returns success. Shared subroutine between
 * get_fullscreen_rect() and get_workingarea_rect(). */
static bool get_monitor_info(WinGuiSeat *wgs, MONITORINFO *mi)
{
    if (p_GetMonitorInfoA && p_MonitorFromWindow) {
        HMONITOR mon;
        mon = p_MonitorFromWindow(wgs->term_hwnd, MONITOR_DEFAULTTONEAREST);
        mi->cbSize = sizeof(*mi);
        p_GetMonitorInfoA(mon, mi);
        return true;
    }
    return false;
}

/* Get the rect/size of a full-screen window on the nearest available
 * monitor in multimon systems; default to something sensible if only
 * one monitor is present. */
static bool get_fullscreen_rect(WinGuiSeat *wgs, RECT *ss)
{
    MONITORINFO mi;
    if (get_monitor_info(wgs, &mi)) {
        /* structure copy */
        *ss = mi.rcMonitor;
        return true;
    }
    return GetClientRect(GetDesktopWindow(), ss);
}

/* Similar to get_fullscreen_rect, but retrieves the working area of the
 * monitor (minus the taskbar) instead of its full extent. */
static bool get_workingarea_rect(WinGuiSeat *wgs, RECT *ss)
{
    MONITORINFO mi;
    if (get_monitor_info(wgs, &mi)) {
        /* structure copy */
        *ss = mi.rcWork;
        return true;
    }
    /* Fallback is the same as get_monitor_rect, which is good _enough_:
     * if the window overlaps the taskbar, that's not too bad a failure. */
    return GetClientRect(GetDesktopWindow(), ss);
}

/*
 * Go full-screen, using the XAML window's full-screen presenter.
 */
static void make_full_screen(WinGuiSeat *wgs)
{
    if (is_full_screen(wgs))
        return;

    /* The state change notification sorts out every session's size
     * and scroll bar */
    wui_set_state(host, WUI_STATE_FULLSCREEN);
}

/*
 * Leave full-screen mode.
 */
static void clear_full_screen(WinGuiSeat *wgs)
{
    if (!is_full_screen(wgs))
        return;

    wui_set_state(host, WUI_STATE_NORMAL);
}

/*
 * Toggle full-screen mode.
 */
static void flip_full_screen(WinGuiSeat *wgs)
{
    if (is_full_screen(wgs))
        clear_full_screen(wgs);
    else
        make_full_screen(wgs);
}

static size_t win_seat_output(Seat *seat, SeatOutputType type,
                              const void *data, size_t len)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    if (autologon_busy(WUI(wgs)))
        return autologon_output(WUI(wgs), data, len);
    return term_data(wgs->term, data, len);
}

static void wintw_unthrottle(TermWin *tw, size_t bufsize)
{
    WinGuiSeat *wgs = container_of(tw, WinGuiSeat, termwin);
    if (wgs->backend)
        backend_unthrottle(wgs->backend, bufsize);
}

static bool win_seat_eof(Seat *seat)
{
    return true;   /* do respond to incoming EOF with outgoing */
}

static SeatPromptResult win_seat_get_userpass_input(Seat *seat, prompts_t *p)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    SeatPromptResult spr;
    spr = cmdline_get_passwd_input(p, &wgs->cmdline_get_passwd_state, true);
    if (spr.kind == SPRK_INCOMPLETE && autologon_prompts(WUI(wgs), p))
        return SPR_OK;
    if (spr.kind == SPRK_INCOMPLETE)
        spr = term_get_userpass_input(wgs->term, p);
    return spr;
}

static void win_seat_set_trust_status(Seat *seat, bool trusted)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    term_set_trust_status(wgs->term, trusted);
}

static bool win_seat_can_set_trust_status(Seat *seat)
{
    return true;
}

static bool win_seat_get_cursor_position(Seat *seat, int *x, int *y)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    term_get_cursor_position(wgs->term, x, y);
    return true;
}

static bool win_seat_get_window_pixel_size(Seat *seat, int *x, int *y)
{
    WinGuiSeat *wgs = container_of(seat, WinGuiSeat, seat);
    RECT r;
    GetWindowRect(wgs->term_hwnd, &r);
    *x = r.right - r.left;
    *y = r.bottom - r.top;
    return true;
}
