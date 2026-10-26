/*
 * wuidlg.h: interface between the C side of the WinUI 3 PuTTY
 * configuration box (winui-config.c, which knows about PuTTY's
 * portable controlbox structures and implements the dlg_* API on top
 * of this) and the C++/WinRT side (winui-dialog.cpp, which owns the
 * XAML window and its controls, and knows nothing about PuTTY).
 *
 * The window looks like the Windows 11 Settings app. The navigation
 * pane on the left has the top-level categories, with icons. The page
 * on the right has a breadcrumb trail for a title, then a card for
 * each group of settings, headed by the group's title, with a row for
 * each setting (its label on the left and its control on the right),
 * and at the end, a card of rows leading to the category's
 * subcategories. The main button (Open, or Apply) stays put beside the
 * page title whichever page is showing.
 *
 * Like wui.h, this must be includable from both C and C++.
 */

#ifndef PUTTY_WINUI_WUIDLG_H
#define PUTTY_WINUI_WUIDLG_H

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

typedef struct WuiDlg WuiDlg;
/* A widget, or a group that widgets go in */
typedef struct WuiWidget WuiWidget;

typedef enum WuiWidgetEvent {
    WUI_EV_FOCUS,       /* the widget gained the input focus */
    WUI_EV_UNFOCUS,     /* ... or lost it */
    WUI_EV_VALCHANGE,   /* the user changed its value */
    WUI_EV_ACTION,      /* button pressed, or list item double-clicked */
    WUI_EV_SELCHANGE,   /* the user changed a list's selection */
    WUI_EV_BROWSE,      /* a file or font selector's button was pressed */
    WUI_EV_COMMIT,      /* an editable combo box lost the focus */
    WUI_EV_RESET,       /* a setting's Reset to default */
    WUI_EV_APPLYALL,    /* a setting's Apply to all saved sessions */
    WUI_EV_DROP,        /* a row was dragged somewhere (see
                         * wui_w_rows_dropped) */
} WuiWidgetEvent;

typedef enum WuiRowKind {
    WUI_ROW_HEADING,    /* a heading between rows */
    WUI_ROW_FOLDER,     /* a row that stands for some others */
    WUI_ROW_ITEM,       /* a row for a thing, with actions */
    WUI_ROW_LINK        /* a row that just does something when clicked */
} WuiRowKind;

/* ----------------------------------------------------------------
 * Provided by winui-dialog.cpp, called from C. Text is always UTF-16.
 */

/* The dialog window, owned by (and disabling, while it runs) 'owner',
 * which may be NULL. 'ctx' is passed back to the callbacks below. */
WuiDlg *wui_dlg_new(HWND owner, const wchar_t *title, HICON icon,
                    void *ctx);
HWND wui_dlg_hwnd(WuiDlg *dlg);
/* Make the window light (theme 1) or dark (2), or follow Windows (0) */
void wui_dlg_set_theme(WuiDlg *dlg, int theme);
/* Show the window and run a nested message loop until wui_dlg_stop */
void wui_dlg_run(WuiDlg *dlg);
void wui_dlg_stop(WuiDlg *dlg);
/* Close the window and free everything in it */
void wui_dlg_free(WuiDlg *dlg);

/* A top-level category in the navigation pane, with an icon (a Segoe
 * Fluent Icons glyph) and an id that wui_dlg_cb_category passes back */
void wui_dlg_add_category(WuiDlg *dlg, const wchar_t *label,
                          unsigned glyph, int id);
/* Highlight a category, without calling wui_dlg_cb_category */
void wui_dlg_select_category(WuiDlg *dlg, int id);
/* A category at the bottom of the navigation pane, below the others */
void wui_dlg_add_footer_category(WuiDlg *dlg, const wchar_t *label,
                                 unsigned glyph, int id);
/* A line above the page title, which stays as the pages change,
 * saying what's being edited: an icon, some text, a dot (if it has
 * changes not yet saved), and some more text after that. NULL text
 * hides it. */
void wui_dlg_set_context(WuiDlg *dlg, unsigned glyph, const wchar_t *text,
                         bool dot, const wchar_t *detail);
/* ... and a tooltip on it: a line of explanation, and a line in
 * italics (the folder a saved session's in, say), either of which may
 * be NULL */
void wui_dlg_set_context_tip(WuiDlg *dlg, const wchar_t *text,
                             const wchar_t *italic);
/* A help button at the end of the title bar, which opens 'url' in the
 * browser (NULL hides it) */
void wui_dlg_set_help(WuiDlg *dlg, const wchar_t *url);

/* The page: remove everything on it (invalidating all its widgets) */
void wui_dlg_clear_page(WuiDlg *dlg);
/* Its title: a breadcrumb trail of n categories, each of which can be
 * clicked on to go to it (through wui_dlg_cb_category), and a
 * subtitle underneath, which may be NULL */
void wui_dlg_page_title(WuiDlg *dlg, int n, const wchar_t *const *crumbs,
                        const int *ids, const wchar_t *subtitle);
/* A group of settings on the page, with an optional heading; or (if
 * 'footer') the buttons that end the dialog, which wui_dlg_clear_page
 * leaves alone */
WuiWidget *wui_dlg_group(WuiDlg *dlg, bool footer, const wchar_t *title);
/* Put the settings added to a group from now on side by side in one
 * row, each with its label above it, a text box taking up the room
 * that's left. (For a few short ones, such as the host box's.) */
void wui_dlg_group_one_row(WuiWidget *grp);
/* ... and go back to a row each for the ones after that */
void wui_dlg_group_end_row(WuiWidget *grp);
/* Let the settings added to a group from now on fill its card edge to
 * edge, a list without a frame of its own. (For a card that's just a
 * list, such as the saved sessions'.) */
void wui_dlg_group_flush(WuiWidget *grp);
/* ... or have no card of its own, for a rows widget, which makes a
 * card of each run of rows between its headings */
void wui_dlg_group_bare(WuiWidget *grp);
/* A row in the card at the end of the page, leading to another
 * category */
void wui_dlg_subpage(WuiDlg *dlg, const wchar_t *label, int id);
/* Buttons beside the page title: one (raising WUI_EV_ACTION, and the
 * app's main colour if 'accent'), or one with a menu of items (added
 * with wui_w_menu_add, each raising WUI_EV_ACTION for its wctx; a NULL
 * label is a separator) */
WuiWidget *wui_dlg_page_button(WuiDlg *dlg, void *wctx, const wchar_t *label,
                               unsigned glyph, bool accent);
WuiWidget *wui_dlg_page_menu(WuiDlg *dlg, const wchar_t *tooltip);
void wui_w_menu_add(WuiWidget *menu, void *wctx, const wchar_t *label,
                    unsigned glyph);
/* A message across the top of the page, perhaps with a button (which
 * raises WUI_EV_ACTION for wctx), until the page changes or (if
 * 'seconds') that long has gone by */
void wui_dlg_notice(WuiDlg *dlg, const wchar_t *text, const wchar_t *action,
                    void *wctx, int seconds, bool error);
/* Ask for some text, in a dialog over the window (starting as
 * 'initial', and not letting any of the characters in 'refused', if
 * that's not NULL, be typed or pasted in); if it's confirmed, raise
 * WUI_EV_ACTION for wctx, and then the text is wui_dlg_prompt_text's */
void wui_dlg_prompt(WuiDlg *dlg, void *wctx, const wchar_t *title,
                    const wchar_t *text, const wchar_t *initial,
                    const wchar_t *refused, const wchar_t *confirm);
size_t wui_dlg_prompt_text(WuiDlg *dlg, wchar_t *buf, size_t size);
/* Ask for a colour, with WinUI's colour picker, in a dialog over the
 * window, starting at *rgb (0xRRGGBB), and wait for it; true, with the
 * colour in *rgb, if one was chosen */
bool wui_dlg_pick_colour(WuiDlg *dlg, const wchar_t *title, unsigned *rgb);

/* Ask for a font, in a dialog over the window, and wait for it: each of
 * the families given, normal and bold, each shown in itself, a size,
 * and a preview. Starts at, and if one's chosen (returning true) puts
 * it in, *family (a buffer of 'size' wchar_ts), *bold and *points. */
bool wui_dlg_pick_font(WuiDlg *dlg, const wchar_t *title,
                       const wchar_t *const *families, size_t nfamilies,
                       wchar_t *family, size_t size, bool *bold,
                       int *points);

/* Give the next wui_dlg_prompt a tree of folders to choose from as
 * well, under the text (starting at 'sel'), and a way to make a new
 * one: the first item is the top of the tree, and the others are paths
 * ("Clients/ACME"), each after the folder it's in; then
 * wui_dlg_prompt_folder is the path of the one chosen (perhaps a new
 * one; "" for the top, or with no tree) */
void wui_dlg_prompt_choices(WuiDlg *dlg, const wchar_t *label,
                            const wchar_t *const *items, size_t n,
                            size_t sel);
size_t wui_dlg_prompt_folder(WuiDlg *dlg, wchar_t *buf, size_t size);
/* Ask a question, in a dialog over the window, and wait for the
 * answer: 1 for the 'yes' button, 2 for 'no' (if there is one), 0 for
 * Cancel; or -1 if it couldn't be asked that way */
int wui_dlg_ask(WuiDlg *dlg, const wchar_t *title, const wchar_t *text,
                const wchar_t *yes, const wchar_t *no);

/*
 * The widgets, each created at the end of a group. 'wctx' is passed
 * back to wui_dlg_cb_widget to say which widget an event is about.
 *
 * Settings with a single value get a row with the label on the left
 * and the control on the right: edit boxes, combo boxes, drop-down
 * lists, file and font selectors, and on/off switches for check
 * boxes. A set of radio buttons becomes a drop-down list too. Lists
 * get a row with the label above them. Buttons in a row go in a row
 * of their own. In the footer (the buttons that end the dialog), the
 * default one goes beside the page title, and a cancel button isn't
 * shown, as Escape and the close button do its job. Text is just text.
 */
WuiWidget *wui_w_text(WuiWidget *grp, void *wctx, const wchar_t *text);
/* An edit box. If 'numeric' is 1 or 2, it's for an integer or a
 * decimal number, which can be stepped by 'step', and then its text
 * is in the C locale (so a decimal point is always '.'). */
WuiWidget *wui_w_edit(WuiWidget *grp, void *wctx, const wchar_t *label,
                      bool password, bool combo, int numeric, double step);
WuiWidget *wui_w_radio(WuiWidget *grp, void *wctx, const wchar_t *label,
                       int nbuttons, const wchar_t *const *buttons);
WuiWidget *wui_w_check(WuiWidget *grp, void *wctx, const wchar_t *label);
WuiWidget *wui_w_button(WuiWidget *grp, void *wctx, const wchar_t *label,
                        bool isdefault, bool iscancel);
/* A list box. If height is 0, it's a drop-down list. Items with tabs
 * in are split into ncols columns of the given percentage widths. A
 * drag list has Up and Down buttons to reorder it. */
WuiWidget *wui_w_list(WuiWidget *grp, void *wctx, const wchar_t *label,
                      int height, bool multisel, bool draglist,
                      int ncols, const int *percentages);
/* A button to the right of an edit box's control, in its row */
WuiWidget *wui_w_button_beside(WuiWidget *edit, void *wctx,
                               const wchar_t *label);
/* An action for each item of a list, which selects the item and then
 * raises WUI_EV_ACTION for 'target' (another widget's wctx, or the
 * list's own): a button in the item if 'primary', or else an entry
 * (with an icon, if 'glyph') in a menu at its end. Must be added
 * before the items are. */
void wui_w_list_action(WuiWidget *list, const wchar_t *label,
                       unsigned glyph, bool primary, void *target);
/*
 * A table of steps, for Automate logon: each row has an on/off check
 * box, the text to wait for, a command, and a check box to hide the
 * command, and buttons to move it up or down (it can be dragged, too)
 * or delete it; and a button below adds a row. 'headers' are the five
 * columns' (the first being the row number's). Any change raises
 * WUI_EV_VALCHANGE, and then the whole table can be read back.
 */
WuiWidget *wui_w_steps(WuiWidget *grp, void *wctx,
                       const wchar_t *const *headers,
                       const wchar_t *add_label);
void wui_w_steps_add(WuiWidget *w, bool active, const wchar_t *expect,
                     const wchar_t *command, bool hide);
int wui_w_steps_count(WuiWidget *w);
/* Column 0 is the text to wait for, and 1 the command */
size_t wui_w_steps_text(WuiWidget *w, int row, int col,
                        wchar_t *buf, size_t size);
/* Column 0 is whether it's on, and 1 whether to hide the command */
bool wui_w_steps_flag(WuiWidget *w, int row, int col);

/*
 * A box to type in, with a button beside it: typing raises
 * WUI_EV_VALCHANGE, and Return or the button WUI_EV_ACTION. Its text
 * is as an edit box's. Down arrow goes to the rows widget 'next'.
 */
WuiWidget *wui_w_search(WuiWidget *grp, void *wctx,
                        const wchar_t *placeholder, const wchar_t *button);
void wui_w_search_next(WuiWidget *search, WuiWidget *next);
/*
 * Rows, each an icon, a title with a subtitle under it, and a detail
 * on the right, indented by some levels, in cards with headings
 * between them (in a bare group, so that the cards are its own); and
 * for items, a menu of actions (also on right-clicking it), each
 * raising WUI_EV_ACTION for 'target' with the item selected (with its
 * alternative label and icon for a 'marked' item, and a key that does
 * it too, or 0). Double-clicking (or Return on) an item, or clicking
 * a folder, raises WUI_EV_ACTION with it selected; and so does
 * clicking a heading with an id (and perhaps an icon, and a detail),
 * though it can't be selected. wui_w_rows_activated says which row
 * that was. 'empty' is what to say if there are
 * no rows. The actions must be added before the rows.
 */
WuiWidget *wui_w_rows(WuiWidget *grp, void *wctx, const wchar_t *empty);
void wui_w_rows_action(WuiWidget *w, const wchar_t *label,
                       const wchar_t *alt_label, unsigned glyph,
                       unsigned alt_glyph, void *target, int key);
/* Actions for folders, as wui_w_rows_action's are for items; and for
 * the list itself, on right-clicking it where there's no row (these
 * don't select anything) */
void wui_w_rows_folder_action(WuiWidget *w, const wchar_t *label,
                              unsigned glyph, void *target, int key);
void wui_w_rows_list_action(WuiWidget *w, const wchar_t *label,
                            unsigned glyph, void *target);
/*
 * Let items and folders be dragged (or stop them): dropping one on
 * another item or folder, or anywhere else in the widget, raises
 * WUI_EV_DROP, and then wui_w_rows_dropped says which row was dropped
 * where (-1 for not on a row).
 */
void wui_w_rows_draggable(WuiWidget *w, bool draggable);
void wui_w_rows_dropped(WuiWidget *w, int *what, int *where);
void wui_w_rows_clear(WuiWidget *w);
void wui_w_rows_add(WuiWidget *w, int id, WuiRowKind kind, int indent,
                    unsigned glyph, const wchar_t *title,
                    const wchar_t *subtitle, const wchar_t *detail,
                    bool marked);
/* A tooltip for the row with that id */
void wui_w_rows_tooltip(WuiWidget *w, int id, const wchar_t *tip);
int wui_w_rows_activated(WuiWidget *w);
/* The selected row's id, or -1 */
int wui_w_rows_selected(WuiWidget *w);
void wui_w_rows_select(WuiWidget *w, int id);

/* A setting that's not what it is by default gets a button to reset
 * it (raising WUI_EV_RESET); and a setting can have a menu, on
 * right-clicking it, to do that (and perhaps to raise WUI_EV_APPLYALL
 * as well) */
void wui_w_set_modified(WuiWidget *w, bool modified);
void wui_w_setting_menu(WuiWidget *w, bool apply_all);
void wui_w_enable(WuiWidget *w, bool enabled);
void wui_w_set_visible(WuiWidget *w, bool visible);

/* Give each item of a list an icon */
void wui_w_list_icon(WuiWidget *list, unsigned glyph);
/*
 * A button beside the page title (before the main button), which asks
 * for some text in a pop-up. Its text is that text, and changing it
 * raises WUI_EV_VALCHANGE, as an edit box's does; pressing the pop-up's
 * 'confirm' button (or Return) raises WUI_EV_ACTION for 'target'.
 */
WuiWidget *wui_w_page_prompt(WuiDlg *dlg, void *wctx, const wchar_t *label,
                             const wchar_t *prompt, const wchar_t *confirm,
                             void *target);
/* A file selector: an edit box with a Browse button, or (if
 * just_button) a button whose label is the label */
WuiWidget *wui_w_filesel(WuiWidget *grp, void *wctx, const wchar_t *label,
                         bool just_button);
/* A font selector: a description of the font under the label, and a
 * Change button */
WuiWidget *wui_w_fontsel(WuiWidget *grp, void *wctx, const wchar_t *label);

/* Text of an edit box, combo box, text widget, file selector or font
 * selector's description. wui_w_get_text returns the length, and
 * writes as much as fits (terminated) into buf. */
void wui_w_set_text(WuiWidget *w, const wchar_t *text);
size_t wui_w_get_text(WuiWidget *w, wchar_t *buf, size_t size);
void wui_w_select_range(WuiWidget *w, size_t start, size_t len);
void wui_w_set_label(WuiWidget *w, const wchar_t *label);
void wui_w_set_tooltip(WuiWidget *w, const wchar_t *tip);
/* Don't let any of these characters be typed or pasted into an edit
 * box */
void wui_w_refuse_chars(WuiWidget *w, const wchar_t *chars);
/* Make a button the app's main colour, or not */
void wui_w_set_accent(WuiWidget *w, bool accent);
void wui_w_set_checked(WuiWidget *w, bool checked);
bool wui_w_get_checked(WuiWidget *w);
void wui_w_set_radio(WuiWidget *w, int which);
int wui_w_get_radio(WuiWidget *w);
void wui_w_focus(WuiWidget *w);

/* Lists, and the drop-down lists of combo boxes */
void wui_w_list_clear(WuiWidget *w);
void wui_w_list_add(WuiWidget *w, const wchar_t *text, int id);
void wui_w_list_del(WuiWidget *w, int index);
int wui_w_list_id(WuiWidget *w, int index);
/* An item's text, as wui_w_get_text gives it */
size_t wui_w_list_text(WuiWidget *w, int index, wchar_t *buf, size_t size);
/* The selected item, or -1 if none (or more than one) is selected */
int wui_w_list_selected(WuiWidget *w);
bool wui_w_list_issel(WuiWidget *w, int index);
void wui_w_list_select(WuiWidget *w, int index);
/* A square of a colour (0xRRGGBB) at the start of a list item */
void wui_w_list_swatch(WuiWidget *w, int index, unsigned rgb);

/*
 * A session's Event Log, in a window of its own, which (unlike the
 * dialog) doesn't stop its owner while it's open. wui_log_set gives it
 * all the events there are now, each a time, a tab, and what happened.
 * When its user closes it, wui_log_cb_closed is called, and then it's
 * for that to call wui_log_free.
 */
typedef struct WuiLog WuiLog;
WuiLog *wui_log_new(HWND owner, const wchar_t *title, HICON icon,
                    int theme, void *ctx);
void wui_log_set(WuiLog *log, size_t n, const wchar_t *const *lines);
/* Bring it to the front */
void wui_log_show(WuiLog *log);
void wui_log_set_theme(WuiLog *log, int theme);
void wui_log_free(WuiLog *log);

/* The About box: modal, returning when it's closed */
typedef struct WuiAbout {
    const wchar_t *title;              /* the window's */
    const wchar_t *name, *version;
    const wchar_t *byline;             /* (may be NULL) */
    const wchar_t *build;              /* how it was built */
    const wchar_t *copyright, *licence;
    const wchar_t *fork_copyright;     /* (above the other; may be NULL) */
    const wchar_t *notice;             /* at the start of the licence (that
                                        * it's not the original; may be
                                        * NULL) */
    const wchar_t *website, *website_label;
} WuiAbout;
void wui_about(HWND owner, HICON icon, int theme, const WuiAbout *about);

/* ----------------------------------------------------------------
 * Provided by winui-config.c, called from C++.
 */

void wui_dlg_cb_category(void *ctx, int id);
/* The user closed the window (title bar button, Alt+F4) */
void wui_dlg_cb_close(void *ctx);
void wui_dlg_cb_widget(void *ctx, void *wctx, WuiWidgetEvent ev);

/* (provided by winui-window.c) */
void wui_log_cb_closed(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* PUTTY_WINUI_WUIDLG_H */
