/*
 * wui.h: interface between the C side of the WinUI 3 PuTTY front end
 * (winui-window.c, which talks to the terminal, backend, Conf etc)
 * and the C++/WinRT side (winui-host.cpp, which owns the XAML
 * window, the Direct2D / DirectWrite renderer and the XAML input
 * events).
 *
 * There is one window (WuiHost), containing a tab for each session
 * (WuiTerm). All the tabs share the same terminal area, so they all
 * have the same size, give or take whether each one has a scroll bar.
 *
 * This header must be includable from both C and C++, so it uses
 * only plain Windows types. All coordinates and sizes are in
 * physical pixels, relative to the top left of a tab's terminal
 * drawing area (its SwapChainPanel), unless otherwise stated.
 */

#ifndef PUTTY_WINUI_WUI_H
#define PUTTY_WINUI_WUI_H

#include <windows.h>
#include <stdbool.h>
#include <stddef.h>

/* The name of this front end, as appname is for the C side (which
 * gets it from windows/CMakeLists.txt), for the C++ side to use */
#ifndef WUI_APP_NAME
#define WUI_APP_NAME L"JoliPuTTY"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WuiHost WuiHost;
typedef struct WuiTerm WuiTerm;

typedef enum WuiWindowState {
    WUI_STATE_NORMAL, WUI_STATE_MINIMISED, WUI_STATE_MAXIMISED,
    WUI_STATE_FULLSCREEN
} WuiWindowState;

typedef enum WuiCursor {
    WUI_CURSOR_IBEAM, WUI_CURSOR_ARROW, WUI_CURSOR_APPSTARTING,
    WUI_CURSOR_WAIT
} WuiCursor;

typedef enum WuiScrollKind {
    WUI_SCROLL_LINEUP, WUI_SCROLL_LINEDOWN, WUI_SCROLL_PAGEUP,
    WUI_SCROLL_PAGEDOWN, WUI_SCROLL_TOP, WUI_SCROLL_BOTTOM,
    WUI_SCROLL_POSITION
} WuiScrollKind;

typedef enum WuiPointerKind {
    WUI_POINTER_PRESS, WUI_POINTER_RELEASE, WUI_POINTER_MOVE,
    WUI_POINTER_WHEEL
} WuiPointerKind;

typedef enum WuiButton {
    WUI_BUTTON_NONE, WUI_BUTTON_LEFT, WUI_BUTTON_MIDDLE, WUI_BUTTON_RIGHT
} WuiButton;

typedef struct WuiPointerEvent {
    WuiPointerKind kind;
    WuiButton button;          /* for PRESS and RELEASE */
    int x, y;
    bool left_down, middle_down, right_down;
    bool shift, ctrl;
    int wheel_delta;           /* for WHEEL, in WHEEL_DELTA units */
    bool wheel_horizontal;
    unsigned long time_ms;
} WuiPointerEvent;

typedef enum WuiMenuKind {
    WUI_MENU_ITEM, WUI_MENU_SEPARATOR, WUI_MENU_SUBMENU, WUI_MENU_END
} WuiMenuKind;

/*
 * A menu is a flat array of these. WUI_MENU_SUBMENU begins a nested
 * menu (with the given label) which runs until the matching
 * WUI_MENU_END. Labels use the Win32 '&' accelerator convention.
 */
typedef struct WuiMenuItem {
    WuiMenuKind kind;
    unsigned id;
    const wchar_t *label;
    bool enabled, checked;
    unsigned icon;             /* a Segoe Fluent Icons glyph, or 0 */
    /* In the context menu, show this (top-level, with an icon) as one
     * of the row of icon buttons along the top */
    bool primary;
    /* Its keyboard shortcut, to show on its right ("Ctrl+F4"), or NULL:
     * just shown, as the shortcut itself is the front end's to do */
    const wchar_t *accel;
} WuiMenuItem;

typedef enum WuiFontQuality {
    WUI_QUALITY_DEFAULT, WUI_QUALITY_ANTIALIASED, WUI_QUALITY_NONANTIALIASED,
    WUI_QUALITY_CLEARTYPE
} WuiFontQuality;

typedef struct WuiFontRequest {
    const wchar_t *name;
    bool bold;                 /* the font itself is bold */
    int charset;
    WuiFontQuality quality;
    /*
     * Either em_height is nonzero, giving the size of the font, or
     * else cell_height is nonzero, in which case the font is sized
     * so that its cell is that many pixels high. If cell_width is
     * nonzero, glyphs are stretched horizontally so that the cell
     * is exactly that wide.
     */
    int em_height;
    int cell_width, cell_height;
} WuiFontRequest;

typedef struct WuiFontMetrics {
    int cell_width, cell_height;
    int ascent;                /* baseline offset from the cell top */
    int strikethrough_y;       /* offset from the cell top */
    bool monospaced;
} WuiFontMetrics;

typedef struct WuiTextRequest {
    const wchar_t *text;
    int len;
    /*
     * Per-UTF-16-unit advance widths, exactly like ExtTextOut's lpDx
     * (so the first half of a surrogate pair has advance 0). The
     * advances are in final pixels, i.e. after the scaling below.
     */
    const int *dx;
    int x, y;                  /* top left of the first character cell */
    RECT clip;
    COLORREF fg;
    bool bold;
    bool shadow_bold;          /* overprint one pixel to the left */
    bool centre;               /* centre each glyph in its advance */
    float scale_x, scale_y;    /* for double-width / double-height */
} WuiTextRequest;

/* ----------------------------------------------------------------
 * Provided by winui-host.cpp, called from C.
 */

/* Start the XAML application. Returns when the application exits. */
int wui_run(void);
/* Ask the XAML application to exit. */
void wui_exit(void);
/* Whether we're running from the MSIX package (windows/msix), which
 * brings the Windows App SDK runtime with it, and gives us our App
 * User Model ID */
bool wui_packaged(void);

/* The window */
HWND wui_hwnd(WuiHost *host);
void wui_show(WuiHost *host);
void wui_close_window(WuiHost *host);
void wui_set_title(WuiHost *host, const wchar_t *title);
/* A message over the window, like a message box: with a 'primary'
 * button (if not NULL) and a 'close' one (or OK). Returns whether
 * the primary button was pressed. */
bool wui_message(WuiHost *host, const wchar_t *title, const wchar_t *text,
                 const wchar_t *primary, const wchar_t *close,
                 bool warning);
/* A security alert over the window (the host key, or a weak
 * algorithm), the same way: its text in items, as the Seat's dialog
 * text has it (paragraphs, things to show as they are, like the
 * fingerprint, and the "More information" keys and values, in an
 * expander under the rest), a 'heading' in red above it (if not NULL),
 * and up to three buttons, none of which is the default. Returns 1 for
 * 'primary', 2 for 'secondary', 0 for 'close' (or Escape), or -1 if it
 * couldn't be shown (another message showing, or the window disabled),
 * for the caller to ask some other way. */
typedef enum WuiAlertItemType {
    WUI_ALERT_PARA, WUI_ALERT_DISPLAY,
    WUI_ALERT_INFO_KEY, WUI_ALERT_INFO_VALUE, WUI_ALERT_INFO_BLOB,
} WuiAlertItemType;
typedef struct WuiAlertItem {
    WuiAlertItemType type;
    const wchar_t *text;
} WuiAlertItem;
typedef struct WuiAlert {
    const wchar_t *title, *heading;
    const WuiAlertItem *items;
    size_t nitems;
    const wchar_t *primary, *secondary, *close;
} WuiAlert;
int wui_alert(WuiHost *host, const WuiAlert *alert);
/* The icon shown at the left of the tab strip, which brings up the
 * window menu when clicked. The caller keeps ownership of it. */
void wui_set_icon(WuiHost *host, HICON icon);
/* Show the window menu, as for clicking the icon: under it, or (for
 * right-clicking the title bar) where the mouse pointer is */
void wui_show_window_menu(WuiHost *host, bool at_pointer);
int wui_get_dpi(WuiHost *host);
WuiWindowState wui_get_state(WuiHost *host);
void wui_set_state(WuiHost *host, WuiWindowState state);
void wui_set_resizable(WuiHost *host, bool resizable, bool maximisable);
void wui_set_topmost(WuiHost *host, bool topmost);
/* Make the window light (theme 1) or dark (2), or follow Windows (0);
 * and give the selected tab its terminal's background colour, or not */
void wui_set_theme(WuiHost *host, int theme, bool tinted_tab);
/* Select the next (dir > 0) or previous (dir < 0) tab, in the order
 * the user sees them. */
void wui_select_adjacent_tab(WuiHost *host, int dir);
/* The n-th tab, from 1; or with 9, the last */
void wui_select_tab_number(WuiHost *host, int n);
/* The saved sessions' pop-up, as for its button beside the tabs */
void wui_show_sessions_popup(WuiHost *host);

/* The tabs. A new tab becomes the selected one. 'ctx' is passed back
 * to all the per-tab callbacks below. */
WuiTerm *wui_term_new(WuiHost *host, void *ctx);
void wui_term_free(WuiTerm *term);
/* A new pane in the same tab as 'beside', which it splits in two:
 * side by side, or (if not) one above the other. It has the focus. */
WuiTerm *wui_term_new_split(WuiHost *host, void *ctx, WuiTerm *beside,
                            bool side_by_side);
/* Whether a terminal shares its tab with others; and the ctxs of all
 * those in its tab (writing up to 'max' of them, returning how many) */
bool wui_term_is_split(WuiTerm *term);
/* Put the keyboard focus in the pane next to this one in its tab, in a
 * direction (dx or dy, -1 or +1); false if there's none that way */
bool wui_term_focus_pane(WuiTerm *term, int dx, int dy);
size_t wui_term_tab_panes(WuiTerm *term, void **ctxs, size_t max);
void wui_term_activate(WuiTerm *term);
/* Move a tab one place left (dir < 0) or right (dir > 0) in the tab
 * strip, if it isn't already at that end */
bool wui_term_can_move(WuiTerm *term, int dir);
void wui_term_move(WuiTerm *term, int dir);
void wui_term_set_title(WuiTerm *term, const wchar_t *title);
/* Something wrong with the session (it's been disconnected, say), as
 * an icon in its tab, with this in the tab's tooltip; or NULL for
 * nothing wrong */
void wui_term_set_status(WuiTerm *term, const wchar_t *status);
/* What the tab's tooltip says double-clicking it does, if not the
 * usual (changing the session's settings); NULL for that */
void wui_term_set_hint(WuiTerm *term, const wchar_t *hint);
/* What the tab says, if not the title: its saved session's name, say
 * (or NULL for the title); and the saved session's full name, if it
 * has one, for the tooltip (above the title) */
/* Let the user rename the term's tab, for now, in place on the tab */
void wui_term_rename_tab(WuiTerm *term);
void wui_term_set_label(WuiTerm *term, const wchar_t *label,
                        const wchar_t *session);
/* The terminal's default background colour, shown in any part of the
 * terminal area not yet drawn (e.g. while the window is resized). */
void wui_term_set_background(WuiTerm *term, COLORREF bg);

void wui_get_panel_size(WuiTerm *term, int *w, int *h);
/* Resize the window so that this tab's terminal area is w x h. */
void wui_set_panel_size(WuiTerm *term, int w, int h);
void wui_set_scrollbar_visible(WuiTerm *term, bool visible);
void wui_set_scrollbar(WuiTerm *term, int total, int start, int page);
/* Find: show the search box over the terminal, and focus it; and say
 * which match is selected (counting from 1) out of how many, or (if
 * total < 0) that the regular expression is no good */
void wui_term_show_search(WuiTerm *term);

/*
 * The pools of text kept to paste again: this tab's (what's been
 * copied in it lately), its saved session's and all sessions'. Shown
 * in a pop-up at (x, y) in the terminal, to paste one of them from, or
 * do something else with it (through wui_cb_pool_action, with its
 * index; or -1, for something done to them all, as WUI_POOL_CLEAR_TAB
 * is). 'session' says whether there's a saved session's pool to keep
 * things in.
 */
enum { WUI_POOL_TAB, WUI_POOL_SESSION, WUI_POOL_COMMON };
enum { WUI_POOL_PASTE, WUI_POOL_KEEP_SESSION, WUI_POOL_KEEP_COMMON,
       WUI_POOL_REMOVE, WUI_POOL_CLEAR_TAB };
typedef struct WuiPoolItem {
    int pool;                          /* a WUI_POOL_TAB etc. */
    const wchar_t *text;
} WuiPoolItem;
void wui_term_show_pool(WuiTerm *term, int x, int y,
                        const WuiPoolItem *items, size_t n, bool session);
void wui_term_search_result(WuiTerm *term, int which, int total);
/* Regular expressions for Find, in std::wregex's ECMAScript syntax.
 * wui_regex_new returns NULL if the pattern is no good. A match found
 * is at text[*start], *mlen long. */
typedef struct WuiRegex WuiRegex;
WuiRegex *wui_regex_new(const wchar_t *pattern, bool ignore_case);
bool wui_regex_find(WuiRegex *re, const wchar_t *text, size_t len,
                    size_t from, size_t *start, size_t *mlen);
void wui_regex_free(WuiRegex *re);
/*
 * Send File and Receive File: the bar over the terminal for sending a
 * file (with its name, and where to, which starts as 'dest', and
 * offers 'recent' as well), or for receiving one (with its path, which
 * starts as 'path'). know_cwd says whether the shell has told us where
 * it is; if not, the bar offers to explain how to make it.
 * wui_term_send_dest fills in the destination or path if the user
 * hasn't typed one (e.g. once the home directory is known).
 * wui_term_send_state says how it's going: 'text' says more, and
 * 'fraction' (0 to 1) is how much has gone, or < 0 if that's not
 * known. wui_term_listing says what's in a remote directory (as asked
 * for by wui_cb_list), for completing what's typed: directories end
 * with '/'.
 */
typedef enum WuiSendState {
    WUI_SEND_READY,                    /* waiting for the user */
    WUI_SEND_BUSY,                     /* sending */
    WUI_SEND_EXISTS,                   /* there already; replace? */
    WUI_SEND_DONE,                     /* sent (the bar soon goes) */
    WUI_SEND_ERROR                     /* failed; try again? */
} WuiSendState;
void wui_term_show_send(WuiTerm *term, const wchar_t *filename,
                        const wchar_t *dest, const wchar_t *const *recent,
                        size_t nrecent, bool know_cwd);
void wui_term_send_dest(WuiTerm *term, const wchar_t *dest);
void wui_term_send_state(WuiTerm *term, WuiSendState state,
                         const wchar_t *text, double fraction);
void wui_term_show_receive(WuiTerm *term, const wchar_t *path,
                           bool know_cwd);
void wui_term_listing(WuiTerm *term, const wchar_t *dir,
                      const wchar_t *const *names, size_t n);
void wui_set_cursor(WuiTerm *term, WuiCursor cursor);
void wui_show_context_menu(WuiTerm *term, int x, int y);
void wui_focus_terminal(WuiTerm *term);

bool wui_set_font(WuiTerm *term, const WuiFontRequest *req,
                  WuiFontMetrics *metrics);
/* Advance width in pixels of a character in the normal font, or 0 if
 * it has no glyph. */
int wui_char_width(WuiTerm *term, unsigned int codepoint);

/* All the drawing functions must be bracketed by these two. */
bool wui_begin_draw(WuiTerm *term);
void wui_end_draw(WuiTerm *term);
void wui_fill_rect(WuiTerm *term, int x, int y, int w, int h, COLORREF c);
void wui_draw_text(WuiTerm *term, const WuiTextRequest *req);
void wui_draw_icon(WuiTerm *term, HICON icon, int x, int y, int w, int h);

/* ----------------------------------------------------------------
 * Provided by winui-window.c, called from C++.
 */

/* Window-level events */

/* The XAML window exists; open the first session in it. */
void wui_cb_launched(WuiHost *host);
/* The user wants to close the window (title bar button, Alt+F4).
 * The front end decides, and calls wui_close_window if appropriate. */
void wui_cb_close_requested(void);
/* The window has gone away. */
void wui_cb_closed(void);
void wui_cb_state_changed(WuiWindowState state);
void wui_cb_dpi_changed(int dpi);
/* The user wants a new session, via the config box. */
void wui_cb_new_tab_requested(void);
/* The user Shift-clicked the '+' button, for a new window. */
void wui_cb_new_window_requested(void);

/* Per-tab events, with the tab's 'ctx' */

void wui_cb_tab_activated(void *ctx);
/* The user clicked a tab's close button. As for the window, the front
 * end decides, and calls wui_term_free if appropriate. */
void wui_cb_tab_close_requested(void *ctx);
/* The user double-clicked a tab */
void wui_cb_tab_double_clicked(void *ctx);
void wui_cb_panel_resized(void *ctx, int w, int h);
void wui_cb_focus(void *ctx, bool focused);
void wui_cb_pointer(void *ctx, const WuiPointerEvent *ev);
void wui_cb_char(void *ctx, wchar_t c);
void wui_cb_scroll(void *ctx, WuiScrollKind kind, int pos);
/* Find: look for some text in the terminal, and select the match
 * 'dir' (-1 or +1) away from the one selected, or (0) the first at
 * or after it */
void wui_cb_search(void *ctx, const wchar_t *text, bool match_case,
                   bool regex, int dir);
void wui_cb_search_closed(void *ctx);
/* Send File and Receive File: send it to 'dest' (replacing what's
 * there, if 'overwrite'); receive the file at 'path'; list what's in
 * a remote directory; show the file received in Explorer; and the
 * bar's been closed (cancelling whatever it was doing) */
void wui_cb_send(void *ctx, const wchar_t *dest, bool overwrite);
void wui_cb_receive(void *ctx, const wchar_t *path);
void wui_cb_list(void *ctx, const wchar_t *dir);
void wui_cb_show_received(void *ctx);
void wui_cb_send_closed(void *ctx);
/* The renderer lost its contents (e.g. GPU device reset). */
void wui_cb_redraw(void *ctx);
/* PuTTY's items in the window icon's menu, for the visible tab */
const WuiMenuItem *wui_cb_get_window_menu(void *ctx, size_t *nitems);
/* The menu for right-clicking the terminal */
const WuiMenuItem *wui_cb_get_terminal_menu(void *ctx, size_t *nitems);
/* Something done in the pools' pop-up (see wui_term_show_pool) */
void wui_cb_pool_action(void *ctx, int index, int action);
void wui_cb_menu_command(void *ctx, unsigned id);
/* The menu for right-clicking the tab; its commands go to
 * wui_cb_menu_command too */
const WuiMenuItem *wui_cb_get_tab_menu(void *ctx, size_t *nitems);
/* The saved sessions, for the button beside the caption buttons: each
 * one's name (in which a '/' separates folders), what it connects to,
 * whether it's a favorite, when it was last used (0 if never), and the
 * command that opens it (through wui_cb_menu_command, for the visible
 * tab) */
typedef struct WuiSession {
    const wchar_t *name, *detail;
    bool favorite;
    long long last_used;
    unsigned id;
} WuiSession;
const WuiSession *wui_cb_get_sessions(size_t *nsessions);
/* Whether a folder of them is open (as it is on the configuration
 * box's Home page, which this changes too), and opening or closing
 * it */
bool wui_cb_session_folder_open(const wchar_t *folder);
void wui_cb_session_folder_toggle(const wchar_t *folder);
/* ... and which of its sections are closed (as on the Home page,
 * again), as a set of these */
#define WUI_SECTION_FAVORITES 1
#define WUI_SECTION_RECENT 2
#define WUI_SECTION_ALL 4
unsigned wui_cb_sessions_hidden(void);
void wui_cb_sessions_set_hidden(unsigned hidden);
/* ... and which it leaves out altogether (which isn't the Home
 * page's business) */
unsigned wui_cb_sessions_omitted(void);
void wui_cb_sessions_set_omitted(unsigned omitted);
/* Connect to a host name typed in there instead, in a new tab or
 * window: "user@host:port", by SSH unless it starts "telnet://" or
 * the like (see session_quick) */
void wui_cb_quick_connect(void *ctx, const wchar_t *target, bool new_window);
/* Added to one of those's command ids: open it in a new window */
#define WUI_MENU_NEW_WINDOW 1

#ifdef __cplusplus
}
#endif

#endif /* PUTTY_WINUI_WUI_H */
