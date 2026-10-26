/*
 * winui-config.c: the PuTTY configuration box, drawn with WinUI 3.
 *
 * This takes the portable description of the box (a controlbox, set
 * up by config.c and windows/config.c), lays it out with the widgets
 * of wuidlg.h, and implements the dlg_* functions on those widgets,
 * by way of the DlgFrontendVtable hook in controls.c. It follows the
 * classic Win32 box (dialog.c and controls.c) in everything that the
 * event handlers can see: which events they get and when, and the
 * fact that only the controls of the visible panel (and the buttons
 * along the bottom) exist at any one time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <time.h>

#include "putty.h"
#include "dialog.h"
#include "putty-rc.h"

#include <commctrl.h>
#include <shobjidl.h>

#include "wuidlg.h"
#include "wui.h"
#include "winui-config.h"
#include "winui-sessions.h"
#include "autologon.h"

/* Category ids, beside the panels' (which are their indices) */
#define HOME_ID 0x10000                /* the Home page */

/* What the box is editing (see set_target) */
enum { TARGET_NEW, TARGET_SESSION, TARGET_DEFAULTS };

/* The things on the Home page, and in the menus beside page
 * titles, which aren't controls config.c knows about: each one's wctx
 * is the address of an element of ConfigBox's acts */
enum {
    ACT_SEARCH, ACT_ROWS,
    ACT_CONNECT, ACT_NEWWIN, ACT_EDIT, ACT_DUPLICATE, ACT_RENAME, ACT_FAVORITE,
    ACT_COPYCMD, ACT_EXPORT, ACT_DELETE,
    ACT_NEW_FOLDER, ACT_NEW_SUBFOLDER, ACT_NEW_HERE, ACT_RENAME_FOLDER,
    ACT_NEW_FOLDER_BESIDE, ACT_SESSION_NAME, ACT_KEYFILE, ACT_CERTFILE,
    ACT_DELETE_FOLDER,
    ACT_NEW, ACT_IMPORT, ACT_EXPORT_ALL, ACT_DEFAULTS, ACT_UNDO,
    ACT_SAVE, ACT_SAVE_AS, ACT_RENAME_TARGET, ACT_DELETE_TARGET, ACT_DISCARD,
    ACT_APPLY_SAVE,
    ACT_PROMPTED,                      /* an answer to ask_name */
    ACT_LIMIT
};
#define ACT(cb, a) ((void *)&(cb)->acts[a])

/* The sections of the Home page's rows, which can be hidden (and
 * are in the title bar's pop-up of sessions too) */
#define SECTION_FAVORITES WUI_SECTION_FAVORITES
#define SECTION_RECENT WUI_SECTION_RECENT
#define SECTION_ALL WUI_SECTION_ALL

/* What ask_name is asking for */
enum { PROMPT_SAVE_AS, PROMPT_RENAME, PROMPT_RENAME_TARGET, PROMPT_DUPLICATE,
       PROMPT_NEW_FOLDER, PROMPT_RENAME_FOLDER };

typedef struct ConfigCtrl {
    dlgcontrol *ctrl;
    WuiWidget *w;
    bool footer;               /* in the bottom bar, not the panel */
    FontSpec *fs;              /* a font selector's font */
    Filename *fn;              /* a button-only file selector's choice,
                                * during its EVENT_ACTION */
} ConfigCtrl;

/*
 * A set of radio buttons whose last one is "Other:", with a drop-down
 * list beside it of the other choices (the connection type, in the
 * host box, is one), is shown as one drop-down list of all of them;
 * this keeps track of what the two controls would be showing.
 */
typedef struct MergedChoice {
    dlgcontrol *radio, *list;  /* NULL if there's none on the page */
    WuiWidget *w;
    int nfixed;                /* radio buttons before "Other:" */
    int radio_sel, list_sel;
    char **names;              /* the list's entries, and their ids */
    int *ids;
    size_t nnames, namesize, idsize;
} MergedChoice;

typedef struct ConfigBox {
    dlgparam dp;               /* what the event handlers get */
    struct controlbox *ctrlbox;
    WuiDlg *dlg;

    ConfigCtrl *ctrls;         /* the controls that currently exist */
    size_t nctrls, ctrlsize;

    const char **paths;        /* the panel for each category id */
    size_t npaths, pathsize;
    int current;               /* the category showing */

    MergedChoice mc;

    /* What's being edited (see set_target) */
    int target;                /* TARGET_* */
    char *target_name;         /* (a saved session's) */
    Conf *saved;               /* it as it was loaded or last saved */
    Conf *defaults, *builtin;  /* the default settings, and PuTTY's own */
    bool dirty;                /* changed since then */
    WuiWidget *save_w;         /* the page's Save button */
    WuiWidget *connect_w;      /* the main button (Connect) */
    WuiWidget *apply_w;        /* mid-session: Apply */
    WuiWidget *name_w;         /* ... and its saved session's name, on
                                * the Session page */
    WuiWidget *keyfile_w, *certfile_w;  /* (see build_credentials) */
    dlgcontrol *colour_list;   /* Window > Colours' list of them, if
                                * that's the page (see colour_swatches) */
    char *launched;            /* the saved session being opened */
    char *session;             /* mid-session: the tab's, if it has one */
    bool session_moved;        /* ... and it's been renamed since */

    /* The Home page */
    WuiWidget *search, *rows;
    SessionInfo *sess;
    size_t nsess;
    char **folders;            /* the folders made to put sessions in
                                * (see folders_get) */
    size_t nfolders;
    char *new_folder;          /* the folder a new session is for */
    char **rowkeys;            /* what each row is: 'S' and a session's
                                * name, 'F' and a folder's, 'Q' and a
                                * host name to connect to, or 'H' and a
                                * heading's SECTION_* */
    size_t nrowkeys, rowkeysize;
    size_t tree_rows;          /* the first of them that's in the tree
                                * under All sessions (or SIZE_MAX, with
                                * no tree, as when searching) */
    unsigned hidden;           /* the SECTION_*s whose sessions are */
    char **collapsed;          /* folders that are */
    size_t ncollapsed, collapsedsize;
    char *undo_name;           /* the session just deleted */
    Conf *undo_conf;
    bool undo_favorite;
    time_t undo_last_used;
    int prompt_kind;           /* what ask_name is asking for */
    char *prompt_for;          /* ... and about which session */
    char acts[ACT_LIMIT];

    WuiWidget *autologon;      /* the table of steps, if it's showing */
    char autologon_changed;    /* (its address is its widget's wctx) */

    dlgcontrol *unfocus_prev;  /* see wui_dlg_cb_widget */
    bool midsession;           /* Change Settings, not a new session */
    bool ca;                   /* the host CA box (see wd_ca_config),
                                * not settings at all */
    bool dest_readonly;        /* ... of one that's connected, so the host
                                * name, port and connection type are
                                * there, but can't be changed */
    bool initialised;
} ConfigBox;

static wchar_t *to_wide(const char *s, bool utf8)
{
    return dup_mb_to_wc(utf8 ? CP_UTF8 : CP_ACP, s ? s : "");
}

/* A session's name, or a folder's path, as the user sees it (see
 * name_shown), in UTF-16 */
static wchar_t *wide_shown(const char *name)
{
    char *shown = name_shown(name);
    wchar_t *w = to_wide(shown, false);
    sfree(shown);
    return w;
}

static const char *leaf_of(const char *name);
static char *parent_of(const char *name);
static char *name_in(const char *folder, const char *leaf);
static void session_renamed(ConfigBox *cb, const char *from, const char *to);

static char *widget_text(WuiWidget *w, bool utf8)
{
    size_t len = wui_w_get_text(w, NULL, 0);
    wchar_t *buf = snewn(len + 1, wchar_t);
    wui_w_get_text(w, buf, len + 1);
    char *ret = dup_wc_to_mb(utf8 ? CP_UTF8 : CP_ACP, buf, "");
    sfree(buf);
    return ret;
}

static ConfigBox *box_of(dlgparam *dp)
{
    return container_of(dp, ConfigBox, dp);
}

static ConfigCtrl *find_ctrl(ConfigBox *cb, dlgcontrol *ctrl)
{
    for (size_t i = 0; i < cb->nctrls; i++)
        if (cb->ctrls[i].ctrl == ctrl)
            return &cb->ctrls[i];
    return NULL;
}

static WuiWidget *find_widget(dlgparam *dp, dlgcontrol *ctrl)
{
    ConfigCtrl *c = find_ctrl(box_of(dp), ctrl);
    return c ? c->w : NULL;
}

static void free_ctrl(ConfigCtrl *c)
{
    if (c->fs)
        fontspec_free(c->fs);
    if (c->fn)
        filename_free(c->fn);
}

/* ----------------------------------------------------------------------
 * A merged radio button set and list (see MergedChoice).
 */

static bool is_merged(ConfigBox *cb, dlgcontrol *ctrl)
{
    return ctrl && (ctrl == cb->mc.radio || ctrl == cb->mc.list);
}

static void mc_clear_names(MergedChoice *mc)
{
    for (size_t i = 0; i < mc->nnames; i++)
        sfree(mc->names[i]);
    mc->nnames = 0;
}

static void mc_forget(MergedChoice *mc)
{
    mc_clear_names(mc);
    sfree(mc->names);
    sfree(mc->ids);
    memset(mc, 0, sizeof(*mc));
}

/* Select whichever entry the two controls between them stand for */
static void mc_show_selection(MergedChoice *mc)
{
    int index = mc->radio_sel < mc->nfixed ? mc->radio_sel :
        mc->list_sel >= 0 ? mc->nfixed + mc->list_sel : -1;
    wui_w_list_select(mc->w, index);
}

/* The entries: the radio buttons, but for "Other:", then the list's */
static void mc_rebuild(MergedChoice *mc)
{
    wui_w_list_clear(mc->w);
    for (int i = 0; i < mc->nfixed; i++) {
        wchar_t *label = to_wide(mc->radio->radio.buttons[i], false);
        wui_w_list_add(mc->w, label, i);
        sfree(label);
    }
    for (size_t i = 0; i < mc->nnames; i++) {
        wchar_t *label = to_wide(mc->names[i], false);
        wui_w_list_add(mc->w, label, mc->ids[i]);
        sfree(label);
    }
    mc_show_selection(mc);
}

/* The user picked an entry */
static void mc_picked(ConfigBox *cb)
{
    MergedChoice *mc = &cb->mc;
    dlgparam *dp = &cb->dp;
    int index = wui_w_list_selected(mc->w);
    if (index < 0)
        return;
    if (index < mc->nfixed) {
        mc->radio_sel = index;
        mc->radio->handler(mc->radio, dp, dp->data, EVENT_VALCHANGE);
    } else {
        /* (The list's handler selects "Other:" itself) */
        mc->list_sel = index - mc->nfixed;
        mc->list->handler(mc->list, dp, dp->data, EVENT_SELCHANGE);
    }
}

/* Is this radio button set, followed by this control, one to merge? */
static bool mergeable(dlgcontrol *radio, dlgcontrol *next)
{
    return radio->type == CTRL_RADIO && radio->radio.nbuttons >= 2 &&
        !strncmp(radio->radio.buttons[radio->radio.nbuttons - 1],
                 "Other", 5) &&
        next && next->type == CTRL_LISTBOX && next->listbox.height == 0 &&
        next->align_next_to == radio;
}

/* ----------------------------------------------------------------------
 * The dlg_* functions, as called through controls.c.
 */

static bool wd_is_visible(dlgcontrol *ctrl, dlgparam *dp)
{
    return find_widget(dp, ctrl) != NULL;
}

static void wd_radiobutton_set(dlgcontrol *ctrl, dlgparam *dp, int which)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl)) {
        cb->mc.radio_sel = which;
        mc_show_selection(&cb->mc);
        return;
    }
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_set_radio(w, which);
}

static int wd_radiobutton_get(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl))
        return cb->mc.radio_sel;
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? wui_w_get_radio(w) : 0;
}

static void wd_checkbox_set(dlgcontrol *ctrl, dlgparam *dp, bool checked)
{
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_set_checked(w, checked);
}

static bool wd_checkbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? wui_w_get_checked(w) : false;
}

static void wd_editbox_set(dlgcontrol *ctrl, dlgparam *dp,
                           const char *text, bool utf8)
{
    WuiWidget *w = find_widget(dp, ctrl);
    if (!w)
        return;
    wchar_t *wtext = to_wide(text, utf8);
    wui_w_set_text(w, wtext);
    sfree(wtext);
}

static char *wd_editbox_get(dlgcontrol *ctrl, dlgparam *dp, bool utf8)
{
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? widget_text(w, utf8) : dupstr("");
}

static void wd_editbox_select_range(dlgcontrol *ctrl, dlgparam *dp,
                                    size_t start, size_t len)
{
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_select_range(w, start, len);
}

static void wd_listbox_clear(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl)) {
        mc_clear_names(&cb->mc);
        cb->mc.list_sel = -1;
        mc_rebuild(&cb->mc);
        return;
    }
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_list_clear(w);
}

static void wd_listbox_del(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl)) {
        MergedChoice *mc = &cb->mc;
        if (index < 0 || (size_t)index >= mc->nnames)
            return;
        sfree(mc->names[index]);
        memmove(mc->names + index, mc->names + index + 1,
                (mc->nnames - index - 1) * sizeof(*mc->names));
        memmove(mc->ids + index, mc->ids + index + 1,
                (mc->nnames - index - 1) * sizeof(*mc->ids));
        mc->nnames--;
        if (mc->list_sel >= (int)mc->nnames)
            mc->list_sel = -1;
        mc_rebuild(mc);
        return;
    }
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_list_del(w, index);
}

static void wd_listbox_addwithid(dlgcontrol *ctrl, dlgparam *dp,
                                 const char *text, int id)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl)) {
        MergedChoice *mc = &cb->mc;
        sgrowarray(mc->names, mc->namesize, mc->nnames);
        sgrowarray(mc->ids, mc->idsize, mc->nnames);
        mc->names[mc->nnames] = dupstr(text);
        mc->ids[mc->nnames] = id;
        mc->nnames++;
        mc_rebuild(mc);
        return;
    }
    WuiWidget *w = find_widget(dp, ctrl);
    if (!w)
        return;
    wchar_t *wtext = to_wide(text, false);
    wui_w_list_add(w, wtext, id);
    sfree(wtext);
}

static int wd_listbox_getid(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl))
        return (index >= 0 && (size_t)index < cb->mc.nnames) ?
            cb->mc.ids[index] : 0;
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? wui_w_list_id(w, index) : 0;
}

static int wd_listbox_index(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl))
        return cb->mc.list_sel;
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? wui_w_list_selected(w) : -1;
}

static bool wd_listbox_issel(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl))
        return index == cb->mc.list_sel;
    WuiWidget *w = find_widget(dp, ctrl);
    return w ? wui_w_list_issel(w, index) : false;
}

static void wd_listbox_select(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    ConfigBox *cb = box_of(dp);
    if (is_merged(cb, ctrl)) {
        cb->mc.list_sel = index;
        mc_show_selection(&cb->mc);
        return;
    }
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_list_select(w, index);
}

static void wd_text_set(dlgcontrol *ctrl, dlgparam *dp, const char *text)
{
    wd_editbox_set(ctrl, dp, text, false);
}

static void wd_label_change(dlgcontrol *ctrl, dlgparam *dp, const char *text)
{
    WuiWidget *w = find_widget(dp, ctrl);
    if (!w)
        return;
    wchar_t *wtext = to_wide(text, false);
    wui_w_set_label(w, wtext);
    sfree(wtext);
}

static void wd_filesel_set(dlgcontrol *ctrl, dlgparam *dp, Filename *fn)
{
    WuiWidget *w = find_widget(dp, ctrl);
    assert(!ctrl->fileselect.just_button);
    if (w)
        wui_w_set_text(w, filename_to_wstr(fn));
}

static Filename *wd_filesel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigCtrl *c = find_ctrl(box_of(dp), ctrl);
    if (!c)
        return filename_from_str("");
    if (ctrl->fileselect.just_button)
        return c->fn ? filename_copy(c->fn) : filename_from_str("");

    size_t len = wui_w_get_text(c->w, NULL, 0);
    wchar_t *buf = snewn(len + 1, wchar_t);
    wui_w_get_text(c->w, buf, len + 1);
    Filename *ret = filename_from_wstr(buf);
    sfree(buf);
    return ret;
}

static void wd_fontsel_set(dlgcontrol *ctrl, dlgparam *dp, FontSpec *fs)
{
    ConfigCtrl *c = find_ctrl(box_of(dp), ctrl);
    if (!c)
        return;
    if (c->fs)
        fontspec_free(c->fs);
    c->fs = fontspec_copy(fs);

    const char *boldstr = (fs->isbold ? ", bold" : "");
    char *buf;
    if (fs->height == 0)
        buf = dupprintf("%s%s, default size", fs->name, boldstr);
    else
        buf = dupprintf("%s%s, %d %s", fs->name, boldstr,
                        (fs->height < 0 ? -fs->height : fs->height),
                        (fs->height < 0 ? "pixels" : "point"));
    wchar_t *wbuf = to_wide(buf, false);
    wui_w_set_text(c->w, wbuf);
    sfree(wbuf);
    sfree(buf);

    dlg_auto_set_fixed_pitch_flag(dp);
}

static FontSpec *wd_fontsel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigCtrl *c = find_ctrl(box_of(dp), ctrl);
    return (c && c->fs) ? fontspec_copy(c->fs) : fontspec_new_default();
}

static void wd_set_focus(dlgcontrol *ctrl, dlgparam *dp)
{
    WuiWidget *w = find_widget(dp, ctrl);
    if (w)
        wui_w_focus(w);
}

static void wd_refresh(dlgcontrol *ctrl, dlgparam *dp)
{
    ConfigBox *cb = box_of(dp);

    if (ctrl) {
        if (ctrl->handler)
            ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
        return;
    }

    /* Everything that exists. (The handlers can't change what that
     * is, so the array stays put while we go through it.) */
    for (size_t i = 0; i < cb->nctrls; i++) {
        dlgcontrol *c = cb->ctrls[i].ctrl;
        if (c->handler)
            c->handler(c, dp, dp->data, EVENT_REFRESH);
    }
}

/* What config.c says is wrong (a port forwarding with no port, say):
 * in a notice across the page, as the box says other things, not a
 * message box */
static void notice(ConfigBox *cb, bool error, const char *fmt, ...);

static void wd_error_msg(dlgparam *dp, const char *msg)
{
    notice(box_of(dp), true, "%s", msg);
}

static void wd_ca_config(dlgparam *dp);

static const DlgFrontendVtable wui_dlg_vt = {
    .is_visible = wd_is_visible,
    .radiobutton_set = wd_radiobutton_set,
    .radiobutton_get = wd_radiobutton_get,
    .checkbox_set = wd_checkbox_set,
    .checkbox_get = wd_checkbox_get,
    .editbox_set = wd_editbox_set,
    .editbox_get = wd_editbox_get,
    .editbox_select_range = wd_editbox_select_range,
    .listbox_clear = wd_listbox_clear,
    .listbox_del = wd_listbox_del,
    .listbox_addwithid = wd_listbox_addwithid,
    .listbox_getid = wd_listbox_getid,
    .listbox_index = wd_listbox_index,
    .listbox_issel = wd_listbox_issel,
    .listbox_select = wd_listbox_select,
    .text_set = wd_text_set,
    .label_change = wd_label_change,
    .filesel_set = wd_filesel_set,
    .filesel_get = wd_filesel_get,
    .fontsel_set = wd_fontsel_set,
    .fontsel_get = wd_fontsel_get,
    .set_focus = wd_set_focus,
    .refresh = wd_refresh,
    .error_msg = wd_error_msg,
    .ca_config = wd_ca_config,
};

/* ----------------------------------------------------------------------
 * Laying out the controls.
 */

/*
 * Whether an edit box is for a number: 1 for an integer and 2 for a
 * decimal, with the step to go up and down in; or 0 if not. The
 * standard handler says, for the settings it looks after. The one
 * exception is the port number in the host box (which is the serial
 * line speed for serial connections), whose handler config.c keeps
 * to itself.
 */
static int edit_numeric(dlgcontrol *ctrl, double *step)
{
    *step = 1;
    if (ctrl->editbox.password || ctrl->editbox.has_list)
        return 0;
    if (ctrl->handler == conf_editbox_handler) {
        const struct conf_editbox_handler_type *type = ctrl->context2.cp;
        if (!type)
            return 0;
        if (type->type == EDIT_INT)
            return 1;
        if (type->type == EDIT_FIXEDPOINT) {
            /* (Stop bits go in halves; a delay in seconds can go in
             * thousandths, but you'd want to step it in whole ones) */
            if (type->denominator > 0 && type->denominator <= 10)
                *step = 1.0 / type->denominator;
            return 2;
        }
        return 0;
    }
    if (ctrl->label && !strcmp(ctrl->label, "Port"))
        return 1;
    return 0;
}

static WuiWidget *make_widget(WuiWidget *grp, dlgcontrol *ctrl)
{
    wchar_t *label = ctrl->label ? to_wide(ctrl->label, false) : NULL;
    WuiWidget *w = NULL;

    switch (ctrl->type) {
      case CTRL_TEXT:
        w = wui_w_text(grp, ctrl, label);
        break;
      case CTRL_EDITBOX: {
        double step;
        int numeric = edit_numeric(ctrl, &step);
        w = wui_w_edit(grp, ctrl, label, ctrl->editbox.password,
                       ctrl->editbox.has_list, numeric, step);
        break;
      }
      case CTRL_RADIO: {
        int n = ctrl->radio.nbuttons;
        wchar_t **buttons = snewn(n, wchar_t *);
        for (int i = 0; i < n; i++)
            buttons[i] = to_wide(ctrl->radio.buttons[i], false);
        w = wui_w_radio(grp, ctrl, label, n,
                        (const wchar_t *const *)buttons);
        for (int i = 0; i < n; i++)
            sfree(buttons[i]);
        sfree(buttons);
        break;
      }
      case CTRL_CHECKBOX:
        w = wui_w_check(grp, ctrl, label);
        break;
      case CTRL_BUTTON:
        w = wui_w_button(grp, ctrl, label, ctrl->button.isdefault,
                         ctrl->button.iscancel);
        break;
      case CTRL_LISTBOX:
        w = wui_w_list(grp, ctrl, label, ctrl->listbox.height,
                       ctrl->listbox.multisel != 0, ctrl->listbox.draglist,
                       ctrl->listbox.ncols, ctrl->listbox.percentages);
        break;
      case CTRL_FILESELECT:
        w = wui_w_filesel(grp, ctrl, label, ctrl->fileselect.just_button);
        break;
      case CTRL_FONTSELECT:
        w = wui_w_fontsel(grp, ctrl, label);
        break;
    }

    sfree(label);
    return w;
}

static void add_ctrl(ConfigBox *cb, dlgcontrol *ctrl, WuiWidget *w,
                     bool footer)
{
    sgrowarray(cb->ctrls, cb->ctrlsize, cb->nctrls);
    ConfigCtrl *c = &cb->ctrls[cb->nctrls++];
    memset(c, 0, sizeof(*c));
    c->ctrl = ctrl;
    c->w = w;
    c->footer = footer;
    if (ctrl->type == CTRL_FONTSELECT)
        c->fs = fontspec_new_default();
}

/* The last element of a panel's path, which is its name */
static const char *path_leaf(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

/* The category id of a panel, or -1 */
static int path_id(ConfigBox *cb, const char *path, size_t len)
{
    for (size_t i = 0; i < cb->npaths; i++)
        if (strlen(cb->paths[i]) == len && !strncmp(cb->paths[i], path, len))
            return (int)i;
    return -1;
}

/* The index of the next real control in a set after the i-th, or
 * the number of controls if there isn't one */
static size_t next_index(struct controlset *s, size_t i)
{
    for (i++; i < s->ncontrols; i++)
        if (s->ctrls[i]->type != CTRL_COLUMNS &&
            s->ctrls[i]->type != CTRL_TABDELAY)
            break;
    return i;
}

static dlgcontrol *next_control(struct controlset *s, size_t i)
{
    i = next_index(s, i);
    return i < s->ncontrols ? s->ctrls[i] : NULL;
}

static bool is_button(dlgcontrol *ctrl, const char *label)
{
    return ctrl->type == CTRL_BUTTON && ctrl->label &&
        !strcmp(ctrl->label, label);
}

/* The saved sessions' controls, which the Home page replaces */
static bool is_saved_sessions(struct controlset *s)
{
    return !strcmp(s->pathname, "Session") && s->boxname &&
        !strcmp(s->boxname, "savedsessions");
}

/*
 * Lay out all the controlsets for a path: a panel, or (if 'footer')
 * the buttons along the bottom.
 *
 * Each controlset becomes a group of settings, under its title if it
 * has one. The columns of the classic box don't come into it: each
 * setting has a row to itself, so they just go one after another;
 * except in the host box, whose three settings share one row, under a
 * shorter title than the classic box's.
 */
static void build_autologon(ConfigBox *cb);
static void build_credentials(ConfigBox *cb);
static void update_state(ConfigBox *cb);

static void build_path(ConfigBox *cb, const char *path, bool footer)
{
    for (int index = -1;
         (index = ctrl_find_path(cb->ctrlbox, path, index)) >= 0 ;) {
        struct controlset *s = cb->ctrlbox->ctrlsets[index];

        if (!s->boxname || s->ncontrols == 0)
            continue;                  /* a title, or nothing in it */
        if (is_saved_sessions(s))
            continue;                  /* (the Home page has them) */

        bool host_box = !footer && !strcmp(s->pathname, "Session") &&
            !strcmp(s->boxname, "hostport");
        wchar_t *title = host_box ? dupwcs(
            cb->dest_readonly ? L"Destination (while connected, this can't "
            L"be changed)" : L"Destination") :
            (*s->boxname && s->boxtitle) ? to_wide(s->boxtitle, false) :
            NULL;
        WuiWidget *grp = wui_dlg_group(cb->dlg, footer, title);
        sfree(title);
        if (host_box)
            wui_dlg_group_one_row(grp);

        size_t first_ctrl = cb->nctrls;
        const char *pending_label = NULL;
        for (size_t i = 0; i < s->ncontrols; i++) {
            dlgcontrol *ctrl = s->ctrls[i];
            if (ctrl->type == CTRL_COLUMNS || ctrl->type == CTRL_TABDELAY)
                continue;
            /* Only the buttons that end the dialog (so not About and
             * Help, which a Windows 11 app wouldn't have there) */
            if (footer && !(ctrl->type == CTRL_BUTTON &&
                            (ctrl->button.isdefault ||
                             ctrl->button.iscancel)))
                continue;

            dlgcontrol *next = next_control(s, i);
            if (!footer && !cb->mc.radio) {
                /*
                 * Radio buttons with an "Other:" list to merge. A text
                 * just before them (which is how the host box labels
                 * its connection type) becomes the merged list's label.
                 */
                if (ctrl->type == CTRL_TEXT && next &&
                    mergeable(next, next_control(
                                  s, (size_t)(next_index(s, i)))) &&
                    !next->label) {
                    pending_label = ctrl->label;
                    continue;
                }
                if (mergeable(ctrl, next)) {
                    MergedChoice *mc = &cb->mc;
                    const char *label = ctrl->label ? ctrl->label :
                        pending_label;
                    wchar_t *wlabel = label ? to_wide(label, false) : NULL;
                    mc->radio = ctrl;
                    mc->list = next;
                    mc->nfixed = ctrl->radio.nbuttons - 1;
                    mc->radio_sel = 0;
                    mc->list_sel = -1;
                    mc->w = wui_w_list(grp, ctrl, wlabel, 0, false, false,
                                       0, NULL);
                    sfree(wlabel);
                    add_ctrl(cb, ctrl, mc->w, false);
                    add_ctrl(cb, next, mc->w, false);
                    i = next_index(s, i);  /* (we've done the list too) */
                    pending_label = NULL;
                    continue;
                }
            }
            pending_label = NULL;

            /* (Window > Colours' red, green and blue, side by side) */
            bool rgb = !footer && ctrl->type == CTRL_EDITBOX &&
                !strcmp(s->pathname, "Window/Colours") && ctrl->label;
            if (rgb && !strcmp(ctrl->label, "Red"))
                wui_dlg_group_one_row(grp);

            WuiWidget *w = make_widget(grp, ctrl);
            if (w)
                add_ctrl(cb, ctrl, w, footer);
            if (w && ctrl->type == CTRL_LISTBOX &&
                !strcmp(s->pathname, "Window/Colours"))
                cb->colour_list = ctrl;
            if (rgb && !strcmp(ctrl->label, "Blue"))
                wui_dlg_group_end_row(grp);
        }
        /* (shown, but greyed out: see dest_readonly) */
        if (host_box && cb->dest_readonly)
            for (size_t i = first_ctrl; i < cb->nctrls; i++)
                wui_w_enable(cb->ctrls[i].w, false);
        /* (Automate logon, after the auto-login user name, which it
         * goes on from: see show_category for when that's not there) */
        if (!footer && !strcmp(s->pathname, "Connection/Data") &&
            !strcmp(s->boxname, "login"))
            build_autologon(cb);
    }
}

/* What a panel's called on screen: as config.c calls it, but for the
 * host CA box's one panel, which it calls just "Main"; and, in the box
 * for a new session (not mid-session), the Session panel, which is
 * just where to connect to, beside the saved sessions' page */
static const char *panel_label(ConfigBox *cb, const char *name)
{
    if (cb->ca && !strcmp(name, "Main"))
        return "Host CAs";
    if (!cb->ca && !cb->midsession && !strcmp(name, "Session"))
        return "Address";
    return name;
}

/* The title of a panel: the trail of categories leading to it, and
 * the panel's own title, if it has one */
static void build_title(ConfigBox *cb, const char *path)
{
    const char *subtitle = NULL;
    for (int index = -1;
         (index = ctrl_find_path(cb->ctrlbox, path, index)) >= 0 ;) {
        struct controlset *s = cb->ctrlbox->ctrlsets[index];
        if (!s->boxname && s->boxtitle) {
            subtitle = s->boxtitle;
            break;
        }
    }

    int n = ctrl_path_elements(path);
    wchar_t **crumbs = snewn(n, wchar_t *);
    int *ids = snewn(n, int);
    const char *p = path;
    for (int i = 0; i < n; i++) {
        const char *end = strchr(p, '/');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char *name = dupprintf("%.*s", (int)len, p);
        crumbs[i] = to_wide(panel_label(cb, name), false);
        sfree(name);
        ids[i] = path_id(cb, path, (size_t)(p + len - path));
        p = end ? end + 1 : p + len;
    }

    wchar_t *wsub = subtitle ? to_wide(subtitle, false) : NULL;
    wui_dlg_page_title(cb->dlg, n, (const wchar_t *const *)crumbs, ids, wsub);
    sfree(wsub);
    for (int i = 0; i < n; i++)
        sfree(crumbs[i]);
    sfree(crumbs);
    sfree(ids);
}

/* Cards for the panels just below this one in the tree */
static void build_subpages(ConfigBox *cb, const char *path)
{
    size_t len = strlen(path);
    for (size_t i = 0; i < cb->npaths; i++) {
        const char *q = cb->paths[i];
        if (strncmp(q, path, len) || q[len] != '/' || strchr(q + len + 1, '/'))
            continue;
        wchar_t *label = to_wide(path_leaf(q), false);
        wui_dlg_subpage(cb->dlg, label, (int)i);
        sfree(label);
    }
}

/*
 * Mid-session, under Automate logon, the SSH key to log in with (and
 * its certificate): config.c has no Auth pages then, as changing them
 * can't change how the session's logged in; but Apply and save can
 * save them, for next time. So, editing the Conf itself, as Automate
 * logon does.
 */
static void build_credentials(ConfigBox *cb)
{
    Conf *conf = (Conf *)cb->dp.data;
    if (conf_get_int(conf, CONF_protocol) != PROT_SSH)
        return;
    WuiWidget *grp = wui_dlg_group(cb->dlg, false, L"SSH credentials");
    cb->keyfile_w = wui_w_filesel(grp, ACT(cb, ACT_KEYFILE),
                                  L"Private key file for authentication",
                                  false);
    wui_w_set_text(cb->keyfile_w,
                   filename_to_wstr(conf_get_filename(conf, CONF_keyfile)));
    cb->certfile_w = wui_w_filesel(grp, ACT(cb, ACT_CERTFILE),
                                   L"Certificate to use with the private "
                                   L"key", false);
    wui_w_set_text(cb->certfile_w, filename_to_wstr(
                       conf_get_filename(conf, CONF_detached_cert)));
}

/* One of those changed (typed in, or with Browse) */
static wchar_t *pick_file(HWND owner, const char *title,
                          const wchar_t *initial, bool save,
                          FilereqFilter filter);

static void credentials_event(ConfigBox *cb, WuiWidget *w, int key,
                              WuiWidgetEvent ev)
{
    if (!w || (ev != WUI_EV_VALCHANGE && ev != WUI_EV_BROWSE))
        return;
    size_t len = wui_w_get_text(w, NULL, 0);
    wchar_t *text = snewn(len + 1, wchar_t);
    wui_w_get_text(w, text, len + 1);
    if (ev == WUI_EV_BROWSE) {
        wchar_t *picked = pick_file(
            cb->dp.hwnd, key == CONF_keyfile ? "Select private key file" :
            "Select certificate file", text, false,
            key == CONF_keyfile ? FILTER_KEY_FILES : FILTER_ALL_FILES);
        sfree(text);
        if (!picked)
            return;
        text = picked;
        wui_w_set_text(w, text);
    }
    Filename *fn = filename_from_wstr(text);
    conf_set_filename((Conf *)cb->dp.data, key, fn);
    filename_free(fn);
    sfree(text);
    update_state(cb);
}

/*
 * Automate logon, on the Connection/Data page after the auto-login user
 * name (or mid-session, on the Connection page): a table of steps,
 * each some text to wait for and a command to send then. config.c
 * knows nothing of it (only this front end carries the steps out), so
 * there are no controls in the controlbox for it: the table edits the
 * Conf itself.
 */
static void build_autologon(ConfigBox *cb)
{
    Conf *conf = (Conf *)cb->dp.data;
    static const wchar_t *const headers[5] = {
        L"#", L"Active", L"Expected", L"Command", L"Hide",
    };

    WuiWidget *grp = wui_dlg_group(cb->dlg, false, L"Automate logon");
    wui_w_text(grp, NULL,
               L"For SSH and Telnet connections: wait for each Expected "
               L"text to arrive, then send its Command, followed by Enter. "
               L"With no Expected text, the command is sent as soon as the "
               L"server has sent anything. A step with no Command does "
               L"nothing. Commands are stored as they are, "
               L"hidden or not, so think twice before putting a password "
               L"in one.");
    cb->autologon = wui_w_steps(grp, &cb->autologon_changed, headers,
                                L"Add step");

    size_t n;
    AutologonStep *steps = autologon_get_steps(conf, &n);
    for (size_t i = 0; i < n; i++) {
        wchar_t *expect = to_wide(steps[i].expect, false);
        char *plain = autologon_command(&steps[i]);
        wchar_t *command = to_wide(plain ? plain : "", false);
        if (plain)
            burnstr(plain);
        wui_w_steps_add(cb->autologon, steps[i].active, expect, command,
                        steps[i].hide);
        sfree(expect);
        sfree(command);
    }
    autologon_free_steps(steps, n);
}

/* The table changed: put what it says in the Conf */
static void autologon_changed(ConfigBox *cb)
{
    WuiWidget *w = cb->autologon;
    if (!w)
        return;
    int n = wui_w_steps_count(w);
    AutologonStep *steps = snewn(n > 0 ? n : 1, AutologonStep);
    for (int i = 0; i < n; i++) {
        for (int col = 0; col < 2; col++) {
            size_t len = wui_w_steps_text(w, i, col, NULL, 0);
            wchar_t *buf = snewn(len + 1, wchar_t);
            wui_w_steps_text(w, i, col, buf, len + 1);
            char *text = dup_wc_to_mb(CP_ACP, buf, "");
            sfree(buf);
            if (col)
                steps[i].command = text;
            else
                steps[i].expect = text;
        }
        steps[i].active = wui_w_steps_flag(w, i, 0);
        steps[i].hide = wui_w_steps_flag(w, i, 1);
        steps[i].stored = false;       /* (it's the command itself now) */
    }
    autologon_set_steps((Conf *)cb->dp.data, steps, n);
    autologon_free_steps(steps, n);
}

/* Switch the panel to the one for a category */
static void build_home(ConfigBox *cb);
static void build_page_actions(ConfigBox *cb);
static void update_state(ConfigBox *cb);
static int ctrl_conf_key(dlgcontrol *ctrl);

static void show_category(ConfigBox *cb, int id)
{
    if (id == cb->current ||
        (id != HOME_ID && (id < 0 || (size_t)id >= cb->npaths)))
        return;

    /* Forget the old panel's controls (keeping the bottom bar's) */
    size_t j = 0;
    for (size_t i = 0; i < cb->nctrls; i++) {
        if (cb->ctrls[i].footer)
            cb->ctrls[j++] = cb->ctrls[i];
        else
            free_ctrl(&cb->ctrls[i]);
    }
    cb->nctrls = j;
    if (cb->dp.focused && !find_ctrl(cb, cb->dp.focused))
        cb->dp.focused = NULL;
    if (cb->dp.lastfocused && !find_ctrl(cb, cb->dp.lastfocused))
        cb->dp.lastfocused = NULL;
    cb->unfocus_prev = NULL;
    mc_forget(&cb->mc);
    cb->autologon = NULL;
    cb->search = cb->rows = cb->save_w = cb->name_w = NULL;
    cb->keyfile_w = cb->certfile_w = NULL;
    cb->colour_list = NULL;
    wui_dlg_clear_page(cb->dlg);

    cb->current = id;
    /* (on the Home page, its search box has a Connect button of
     * its own, beside what's typed, so the main one would be a second) */
    if (cb->connect_w)
        wui_w_set_visible(cb->connect_w, id != HOME_ID);
    if (id == HOME_ID) {
        build_home(cb);
        wui_dlg_select_category(cb->dlg, HOME_ID);
        update_state(cb);
        return;
    }

    const char *path = cb->paths[id];
    build_title(cb, path);
    if (!cb->ca)
        build_page_actions(cb);
    /* (on SSH > Auth, its subpages first, Credentials, for the key to
     * log in with, and GSSAPI, being what's most often wanted there) */
    bool subpages_first = !strcmp(path, "Connection/SSH/Auth");
    /* (mid-session, the tab's saved session's name, to be changed
     * here as well as by Rename on the menu) */
    if (cb->midsession && cb->session && !strcmp(path, "Session")) {
        WuiWidget *grp = wui_dlg_group(cb->dlg, false, L"Saved session");
        cb->name_w = wui_w_edit(grp, ACT(cb, ACT_SESSION_NAME), L"Name",
                                false, false, 0, 0);
        wui_w_refuse_chars(cb->name_w, NAME_REFUSED_W);
        wchar_t *leaf = to_wide(leaf_of(cb->session), false);
        wui_w_set_text(cb->name_w, leaf);
        sfree(leaf);
    }
    if (subpages_first)
        build_subpages(cb, path);
    build_path(cb, path, false);
    /* (mid-session, there's no Connection/Data page, as the classic box
     * has nothing there to change then; but Automate logon can still be
     * changed, for Apply and save, so it goes on the Connection page) */
    if (cb->midsession && !strcmp(path, "Connection") && !cb->autologon) {
        build_autologon(cb);
        build_credentials(cb);
    }
    if (!subpages_first)
        build_subpages(cb, path);

    /* The settings we can tell the defaults of can be reset to them */
    for (size_t i = 0; i < cb->nctrls; i++)
        if (!cb->ctrls[i].footer && cb->ctrls[i].w &&
            ctrl_conf_key(cb->ctrls[i].ctrl) >= 0)
            wui_w_setting_menu(cb->ctrls[i].w, true);

    /* Highlight the top-level category this is under */
    const char *slash = strchr(path, '/');
    int top = path_id(cb, path, slash ? (size_t)(slash - path) : strlen(path));
    wui_dlg_select_category(cb->dlg, top >= 0 ? top : id);

    dlg_refresh(NULL, &cb->dp);        /* set up control values */
    update_state(cb);
}

/* Icons for the top-level categories */
static unsigned category_glyph(const char *name)
{
    static const struct { const char *name; unsigned glyph; } glyphs[] = {
        { "Session", 0xE703 },         /* Connect */
        { "Terminal", 0xE756 },        /* CommandPrompt */
        { "Window", 0xE8A7 },          /* OpenInNewWindow */
        { "Connection", 0xE774 },      /* Globe */
        { "Main", 0xEB95 },            /* Certificate (the host CA box) */
    };
    for (size_t i = 0; i < lenof(glyphs); i++)
        if (!strcmp(name, glyphs[i].name))
            return glyphs[i].glyph;
    return 0xE713;                     /* Setting */
}

/* The panels, in the order the classic tree view has them; and the
 * top-level ones in the navigation pane */
static void build_categories(ConfigBox *cb)
{
    const char *prev = NULL;

    for (size_t i = 0; i < cb->ctrlbox->nctrlsets; i++) {
        struct controlset *s = cb->ctrlbox->ctrlsets[i];
        if (!s->pathname[0])
            continue;
        if (prev && ctrl_path_compare(s->pathname, prev) == INT_MAX)
            continue;                  /* same panel as the last one */

        sgrowarray(cb->paths, cb->pathsize, cb->npaths);
        int id = (int)cb->npaths;
        cb->paths[cb->npaths++] = s->pathname;
        if (!strchr(s->pathname, '/')) {
            wchar_t *label = to_wide(panel_label(cb, s->pathname), false);
            wui_dlg_add_category(cb->dlg, label,
                                 category_glyph(s->pathname), id);
            sfree(label);
        }
        prev = s->pathname;
    }
}

/* ----------------------------------------------------------------------
 * What's being edited, and the Home page.
 *
 * The box edits one thing at a time (its 'target'): a new session,
 * not saved yet; a saved session; or the default settings, which are
 * where new sessions start from. It says which, above the page title,
 * with a dot if it's been changed since it was loaded or last saved,
 * and beside the title are the buttons to save it and so on.
 *
 * The Home page, which the box starts on, is for connecting: a
 * box to type a host name in (or to search the saved sessions), and
 * the saved sessions, favorite ones and recently used ones first, then
 * all of them (with the parts of a name before a '#' as folders), each
 * with a menu of things to do with it. Clicking one connects to it.
 *
 * Settings that aren't what they are by default (in the default
 * settings, or when editing those, in PuTTY's own defaults) have a
 * button beside them to reset them, and a menu (on right-clicking) to
 * do that, or to put the setting in every saved session.
 */

static Conf *box_conf(ConfigBox *cb)
{
    return (Conf *)cb->dp.data;
}

/* Ask: returns 1 for 'yes', 2 for 'no' (if there's that button), or 0
 * for Cancel. (With Windows's task dialog, if it can't be done in the
 * box itself.) */
typedef HRESULT (WINAPI *TaskDialogIndirect_t)(
    const TASKDIALOGCONFIG *, int *, int *, BOOL *);

static int ask(ConfigBox *cb, const wchar_t *main, const wchar_t *text,
               const wchar_t *yes, const wchar_t *no)
{
    /* (in the box's own style and theme, if we can) */
    int answer = wui_dlg_ask(cb->dlg, main, text, yes, no);
    if (answer >= 0)
        return answer;

    static TaskDialogIndirect_t tdi;
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE comctl = LoadLibraryW(L"comctl32.dll");
        if (comctl)
            tdi = (TaskDialogIndirect_t)(void (*)(void))GetProcAddress(
                comctl, "TaskDialogIndirect");
    }
    if (tdi) {
        TASKDIALOG_BUTTON buttons[2] = { { 100, yes }, { 101, no } };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = cb->dp.hwnd;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
            TDF_POSITION_RELATIVE_TO_WINDOW;
        tc.pszWindowTitle = WUI_APP_NAME;
        tc.pszMainInstruction = main;
        tc.pszContent = text;
        tc.pButtons = buttons;
        tc.cButtons = no ? 2 : 1;
        tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        tc.nDefaultButton = 100;
        int button = 0;
        if (SUCCEEDED(tdi(&tc, &button, NULL, NULL)))
            return button == 100 ? 1 : button == 101 ? 2 : 0;
    }
    wchar_t *msg = dupwcscat(main, L"\n\n", text);
    int r = MessageBoxW(cb->dp.hwnd, msg, WUI_APP_NAME,
                        (no ? MB_YESNOCANCEL : MB_OKCANCEL) | MB_ICONQUESTION);
    sfree(msg);
    return (r == IDYES || r == IDOK) ? 1 : r == IDNO ? 2 : 0;
}

static void notice(ConfigBox *cb, bool error, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);
    wchar_t *wmsg = to_wide(msg, false);
    wui_dlg_notice(cb->dlg, wmsg, NULL, NULL, error ? 0 : 6, error);
    sfree(wmsg);
    sfree(msg);
}

static void copy_to_clipboard(HWND hwnd, const wchar_t *text)
{
    size_t size = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!mem)
        return;
    memcpy(GlobalLock(mem), text, size);
    GlobalUnlock(mem);
    if (OpenClipboard(hwnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, mem))
            mem = NULL;
        CloseClipboard();
    }
    if (mem)
        GlobalFree(mem);
}

/* Rebuild the page showing, as after loading new settings into the
 * Conf (which some of its widgets only read when they're made) */
static void rebuild_page(ConfigBox *cb)
{
    int id = cb->current;
    cb->current = -1;
    show_category(cb, id);
}

static int session_page(ConfigBox *cb)
{
    int id = path_id(cb, "Session", 7);
    return id >= 0 ? id : 0;
}

/* ---- The target ---- */

static Conf *baseline(ConfigBox *cb)
{
    return cb->target == TARGET_DEFAULTS ? cb->builtin : cb->defaults;
}

/* (config.c has this, but doesn't say so in putty.h) */
void conf_radiobutton_bool_handler(dlgcontrol *ctrl, dlgparam *dlg,
                                   void *data, int event);

/* Which setting a control is for, if it's one of the standard ones */
static int ctrl_conf_key(dlgcontrol *ctrl)
{
    int key;
    if (ctrl->handler == conf_radiobutton_handler ||
        ctrl->handler == conf_radiobutton_bool_handler ||
        ctrl->handler == conf_editbox_handler ||
        ctrl->handler == conf_filesel_handler ||
        ctrl->handler == conf_fontsel_handler)
        key = ctrl->context.i;
    else if (ctrl->handler == conf_checkbox_handler)
        key = ctrl->context.i & ~CHECKBOX_INVERT;
    else
        return -1;
    if (key < 0 || key >= N_CONFIG_OPTIONS ||
        conf_key_info[key].subkey_type != CONF_TYPE_NONE)
        return -1;
    return key;
}

static void update_markers(ConfigBox *cb)
{
    Conf *base = baseline(cb);
    for (size_t i = 0; i < cb->nctrls; i++) {
        ConfigCtrl *c = &cb->ctrls[i];
        int key;
        if (c->footer || !c->w || (key = ctrl_conf_key(c->ctrl)) < 0)
            continue;
        wui_w_set_modified(c->w, !conf_key_equal(box_conf(cb), base, key));
    }
}

/* Say which saved session's being edited: just its name, with the
 * folder it's in (if any) in the tooltip, as on the Home page, under
 * any explanation */
static void set_context_session(ConfigBox *cb, unsigned glyph,
                                const char *session, bool dot,
                                const wchar_t *detail, const wchar_t *tip)
{
    const char *slash = strrchr(session, FOLDER_SEP);
    wchar_t *name = to_wide(slash ? slash + 1 : session, false);
    wui_dlg_set_context(cb->dlg, glyph, name, dot, detail);
    sfree(name);
    if (slash) {
        char *path = dupprintf("%.*s", (int)(slash - session), session);
        char *folder = name_shown(path);
        sfree(path);
        wchar_t *wfolder = to_wide(folder, false);
        wui_dlg_set_context_tip(cb->dlg, tip, wfolder);
        sfree(wfolder);
        sfree(folder);
    } else {
        wui_dlg_set_context_tip(cb->dlg, tip, NULL);
    }
}

/* Say what's being edited, and whether it's been changed */
/* On Window > Colours, each colour in the list beside its name (its
 * items being the colours in order, as config.c's colour_handler adds
 * them) */
static void colour_swatches(ConfigBox *cb)
{
    ConfigCtrl *c = cb->colour_list ? find_ctrl(cb, cb->colour_list) : NULL;
    if (!c || !c->w)
        return;
    Conf *conf = (Conf *)cb->dp.data;
    for (int i = 0; i < CONF_NCOLOURS; i++)
        wui_w_list_swatch(c->w, i,
                          (conf_get_int_int(conf, CONF_colours, i*3+0) << 16) |
                          (conf_get_int_int(conf, CONF_colours, i*3+1) << 8) |
                          conf_get_int_int(conf, CONF_colours, i*3+2));
}

static void update_state(ConfigBox *cb)
{
    colour_swatches(cb);
    if (cb->ca)
        return;                        /* (nothing's being edited) */
    if (cb->midsession) {
        /* (with a dot, and saying so, once there are changes, as when
         * editing a saved session) */
        /* (the explanation in the tooltip, to keep the title bar short) */
        bool dirty = !conf_equal(box_conf(cb), cb->saved);
        const wchar_t *detail = dirty ? L"Changes not saved" : NULL;
        static const wchar_t tip[] =
            L"Changes apply to this session only, unless you save them";
        if (cb->current != HOME_ID)
            update_markers(cb);
        if (cb->session) {
            set_context_session(cb, 0xE80F, cb->session, dirty, detail, tip);
        } else {
            wui_dlg_set_context(cb->dlg, 0xE80F, L"This session", dirty,
                                detail);
            wui_dlg_set_context_tip(cb->dlg, tip, NULL);
        }
        return;
    }
    wui_dlg_set_context_tip(cb->dlg, NULL, NULL);

    cb->dirty = !conf_equal(box_conf(cb), cb->saved);
    if (cb->save_w)
        wui_w_enable(cb->save_w, cb->dirty);
    if (cb->current == HOME_ID) {
        wui_dlg_set_context(cb->dlg, 0, NULL, false, NULL);
        return;
    }
    update_markers(cb);

    switch (cb->target) {
      case TARGET_NEW:
        wui_dlg_set_context(cb->dlg, 0xE710, L"New session", true,
                            L"Not saved");
        break;
      case TARGET_DEFAULTS:
        wui_dlg_set_context(cb->dlg, 0xE713, L"Defaults for new sessions",
                            cb->dirty, cb->dirty ? L"Changes not saved" :
                            NULL);
        wui_dlg_set_context_tip(cb->dlg, L"Where new sessions start from: "
                                L"changing them doesn't change the saved "
                                L"sessions", NULL);
        break;
      default:
        set_context_session(cb, 0xE70F, cb->target_name, cb->dirty,
                            cb->dirty ? L"Changes not saved" : NULL, NULL);
        break;
    }
}

/* Load what's to be edited into the Conf */
static void set_target(ConfigBox *cb, int target, const char *name)
{
    Conf *conf = box_conf(cb);
    Conf *c = conf_new();
    if (target == TARGET_NEW)
        conf_copy_into(c, cb->defaults);
    else
        load_settings(target == TARGET_DEFAULTS ? DEFAULTS_NAME : name, c);
    conf_copy_into(conf, c);
    conf_free(c);

    cb->target = target;
    sfree(cb->new_folder);
    cb->new_folder = NULL;
    char *dup = name ? dupstr(name) : NULL;
    sfree(cb->target_name);
    cb->target_name = dup;
    conf_copy_into(cb->saved, conf);
    dlg_auto_set_fixed_pitch_flag(&cb->dp);
}

/* Before replacing what's being edited: if it's been changed, ask
 * whether to save it first. False means don't go on. */
static bool save_target(ConfigBox *cb);

static bool check_dirty(ConfigBox *cb)
{
    if (cb->midsession || !cb->dirty)
        return true;
    if (cb->target == TARGET_NEW)
        return ask(cb, L"Discard the new session?",
                   L"It hasn't been saved, so its settings will be lost.",
                   L"Discard", NULL) == 1;
    wchar_t *name = cb->target == TARGET_DEFAULTS ?
        dupwcs(L"the defaults for new sessions") :
        wide_shown(cb->target_name);
    wchar_t *main = dupwcscat(L"Save the changes to ", name, L"?");
    int r = ask(cb, main, L"Some settings have been changed since it was "
                L"last saved.", L"Save", L"Don't save");
    sfree(main);
    sfree(name);
    return r == 2 || (r == 1 && save_target(cb));
}

/* Edit something else, and go to its Session page */
static void edit(ConfigBox *cb, int target, const char *name)
{
    if (!check_dirty(cb))
        return;
    set_target(cb, target, name);
    cb->current = -1;
    show_category(cb, session_page(cb));
}

static void reload_sessions(ConfigBox *cb)
{
    sessions_free(cb->sess, cb->nsess);
    cb->sess = sessions_get(&cb->nsess);
    folders_free(cb->folders, cb->nfolders);
    cb->folders = folders_get(&cb->nfolders);
}

/* Save what's being edited where it came from */
static bool save_target(ConfigBox *cb)
{
    Conf *conf = box_conf(cb);
    const char *name = cb->target == TARGET_DEFAULTS ? DEFAULTS_NAME :
        cb->target_name;
    if (!name)
        return false;
    char *err = session_store(name, conf);
    if (err) {
        notice(cb, true, "%s", err);
        sfree(err);
        return false;
    }
    conf_copy_into(cb->saved, conf);
    if (cb->target == TARGET_DEFAULTS)
        conf_copy_into(cb->defaults, conf);
    reload_sessions(cb);
    update_state(cb);
    return true;
}

/* A name typed in for a session: is it one we can use (and if it's
 * already there, does the user want it replaced)? */
static bool name_ok(ConfigBox *cb, const char *name, const char *except)
{
    if (!*name || !strcmp(name, DEFAULTS_NAME)) {
        notice(cb, true, "\"%s\" can't be the name of a session", name);
        return false;
    }
    if ((except && !strcmp(name, except)) || !session_exists(name))
        return true;
    wchar_t *wname = wide_shown(name);
    wchar_t *main = dupwcscat(L"Replace ", wname, L"?");
    bool ok = ask(cb, main, L"There's already a saved session with that "
                  L"name.", L"Replace", NULL) == 1;
    sfree(main);
    sfree(wname);
    return ok;
}

/* ---- Connecting ---- */

static void connect_now(ConfigBox *cb, const char *name)
{
    sfree(cb->launched);
    cb->launched = name ? dupstr(name) : NULL;
    session_touch(name);
    dlg_end(&cb->dp, 1);
    wui_dlg_stop(cb->dlg);
}

static void connect_session(ConfigBox *cb, const char *name)
{
    if (!check_dirty(cb))
        return;
    set_target(cb, TARGET_SESSION, name);
    if (!conf_launchable(box_conf(cb))) {
        /* (it can't be connected to as it is: say why, and let it be
         * put right) */
        cb->current = -1;
        show_category(cb, session_page(cb));
        notice(cb, true, "\"%s\" has no host name to connect to", name);
        return;
    }
    connect_now(cb, name);
}

/* Connect to what's been typed: [user@]host[:port] */
/* What connecting to a host name typed in would be, to say so under
 * it: by SSH, or by what it starts with ("telnet://host"), as
 * session_quick takes it (to free) */
static wchar_t *quick_how(const char *text)
{
    const char *p = text + strspn(text, " \t");
    const char *sep = strstr(p, "://");
    if (!sep)
        return dupwcs(L"As a new session, by SSH (or telnet://..., "
                      L"serial://..., and so on)");
    char *scheme = dupprintf("%.*s", (int)(sep - p), p);
    for (char *q = scheme; *q; q++)
        *q = tolower((unsigned char)*q);
    const BackendVtable *vt = backend_vt_from_name(scheme);
    wchar_t *ret;
    if (vt) {
        wchar_t *name = to_wide(vt->displayname_tc, false);
        ret = dupwcscat(L"As a new session, by ", name);
        sfree(name);
    } else {
        wchar_t *wscheme = to_wide(scheme, false);
        ret = dupwcscat(L"\"", wscheme, L"\" isn't a kind of connection "
                        L"this knows: ssh, telnet, rlogin, raw, serial...");
        sfree(wscheme);
    }
    sfree(scheme);
    return ret;
}

static void quick_connect(ConfigBox *cb, const char *text)
{
    if (!check_dirty(cb))
        return;
    set_target(cb, TARGET_NEW, NULL);
    Conf *conf = box_conf(cb);
    char *err = session_quick(conf, text);
    if (!err && !conf_launchable(conf))
        err = dupstr("There's no host name to connect to");
    if (err) {
        notice(cb, true, "%s", err);
        sfree(err);
        return;
    }
    connect_now(cb, NULL);
}

/* ---- The Home page ---- */

static int add_rowkey(ConfigBox *cb, char kind, const char *s)
{
    sgrowarray(cb->rowkeys, cb->rowkeysize, cb->nrowkeys);
    char k[2] = { kind, '\0' };
    cb->rowkeys[cb->nrowkeys] = dupcat(k, s);
    return (int)cb->nrowkeys++;
}

static void clear_rowkeys(ConfigBox *cb)
{
    for (size_t i = 0; i < cb->nrowkeys; i++)
        sfree(cb->rowkeys[i]);
    cb->nrowkeys = 0;
}

/* The session in a row (or the selected one, if id < 0), or NULL */
static const char *row_session(ConfigBox *cb, int id)
{
    if (id < 0 && cb->rows)
        id = wui_w_rows_selected(cb->rows);
    if (id < 0 || (size_t)id >= cb->nrowkeys || cb->rowkeys[id][0] != 'S')
        return NULL;
    return cb->rowkeys[id] + 1;
}

/* ... and the folder (the selected one's, if id < 0), or NULL */
static const char *row_folder(ConfigBox *cb, int id)
{
    if (id < 0 && cb->rows)
        id = wui_w_rows_selected(cb->rows);
    if (id < 0 || (size_t)id >= cb->nrowkeys || cb->rowkeys[id][0] != 'F')
        return NULL;
    return cb->rowkeys[id] + 1;
}

static SessionInfo *find_session(ConfigBox *cb, const char *name)
{
    for (size_t i = 0; i < cb->nsess; i++)
        if (!strcmp(cb->sess[i].name, name))
            return &cb->sess[i];
    return NULL;
}

/* When it was last used: "5 min ago", "yesterday", ... */
static wchar_t *ago_text(time_t t)
{
    if (!t)
        return NULL;
    wchar_t buf[64];
    long long secs = (long long)(time(NULL) - t);
    if (secs < 60)
        wcscpy(buf, L"just now");
    else if (secs < 3600)
        swprintf(buf, lenof(buf), L"%d min ago", (int)(secs / 60));
    else if (secs < 86400)
        swprintf(buf, lenof(buf), L"%d h ago", (int)(secs / 3600));
    else if (secs < 2 * 86400)
        wcscpy(buf, L"yesterday");
    else if (secs < 7 * 86400)
        swprintf(buf, lenof(buf), L"%d days ago", (int)(secs / 86400));
    else {
        struct tm *tm = localtime(&t);
        if (!tm || !wcsftime(buf, lenof(buf), L"%d %b %Y", tm))
            return NULL;
    }
    return dupwcs(buf);
}

/* How add_session_row shows a session's name: as it is, or just the
 * part after its folder (in the tree, under the folder; or under
 * Favorites or Recent, with the folder in a tooltip) */
enum { NAME_FULL, NAME_IN_TREE, NAME_IN_SECTION };

static void add_session_row(ConfigBox *cb, const SessionInfo *si,
                            int how, int indent)
{
    int id = add_rowkey(cb, 'S', si->name);
    const char *slash = strrchr(si->name, FOLDER_SEP);
    const char *leaf = slash && how != NAME_FULL ? slash + 1 : NULL;
    wchar_t *title = leaf ? to_wide(leaf, false) : wide_shown(si->name);
    wchar_t *sub = session_subtitle(si);
    /* (under Favorites or Recent, the folder it's in goes after what it
     * connects to, as well as in the tooltip) */
    if (how == NAME_IN_SECTION && slash) {
        char *path = dupprintf("%.*s", (int)(slash - si->name), si->name);
        char *folder = name_shown(path);
        sfree(path);
        wchar_t *wfolder = to_wide(folder, false);
        wchar_t *both = sub && *sub ?
            dupwcscat(sub, L"  \u00B7  ", wfolder) : dupwcs(wfolder);
        sfree(sub);
        sub = both;
        sfree(wfolder);
        sfree(folder);
    }
    wchar_t *when = ago_text(si->last_used);
    wui_w_rows_add(cb->rows, id, WUI_ROW_ITEM, indent,
                   si->favorite ? 0xE735 : 0xE756,   /* FavoriteStarFill,
                                                    * CommandPrompt */
                   title, sub, when, si->favorite);
    if (how == NAME_IN_SECTION) {
        char *path = slash ?
            dupprintf("%.*s", (int)(slash - si->name), si->name) : NULL;
        char *folder = path ? name_shown(path) : NULL;
        sfree(path);
        wchar_t *wtip = to_wide(folder ? folder : "(Top level)", false);
        wui_w_rows_tooltip(cb->rows, id, wtip);
        sfree(wtip);
        sfree(folder);
    }
    sfree(when);
    sfree(sub);
    sfree(title);
}

static bool is_collapsed(ConfigBox *cb, const char *folder)
{
    for (size_t i = 0; i < cb->ncollapsed; i++)
        if (!strcmp(cb->collapsed[i], folder))
            return true;
    return false;
}

/* Remember which folders are closed, for next time */
static void save_collapsed(ConfigBox *cb)
{
    home_collapsed_set(cb->collapsed, cb->ncollapsed);
}

/* ... and find out which were last time (of the ones still there) */
static void load_collapsed(ConfigBox *cb)
{
    size_t n;
    char **f = home_collapsed_get(&n);
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(f[i]);
        bool there = false;
        for (size_t j = 0; j < cb->nsess && !there; j++)
            there = !strncmp(cb->sess[j].name, f[i], len) &&
                cb->sess[j].name[len] == FOLDER_SEP;
        for (size_t j = 0; j < cb->nfolders && !there; j++)
            there = !strncmp(cb->folders[j], f[i], len) &&
                (!cb->folders[j][len] ||
                 cb->folders[j][len] == FOLDER_SEP);
        if (there && !is_collapsed(cb, f[i])) {
            sgrowarray(cb->collapsed, cb->collapsedsize, cb->ncollapsed);
            cb->collapsed[cb->ncollapsed++] = f[i];
            f[i] = NULL;
        }
    }
    folders_free(f, n);
}

/* Show what's in a folder, and in the folders it's in */
static void expand_folder(ConfigBox *cb, const char *folder)
{
    for (size_t i = 0; i < cb->ncollapsed; ) {
        size_t len = strlen(cb->collapsed[i]);
        if (!strncmp(folder, cb->collapsed[i], len) &&
            (!folder[len] || folder[len] == FOLDER_SEP)) {
            sfree(cb->collapsed[i]);
            cb->collapsed[i] = cb->collapsed[--cb->ncollapsed];
        } else {
            i++;
        }
    }
    save_collapsed(cb);
}

static void toggle_folder(ConfigBox *cb, const char *folder)
{
    for (size_t i = 0; i < cb->ncollapsed; i++) {
        if (!strcmp(cb->collapsed[i], folder)) {
            sfree(cb->collapsed[i]);
            cb->collapsed[i] = cb->collapsed[--cb->ncollapsed];
            save_collapsed(cb);
            return;
        }
    }
    sgrowarray(cb->collapsed, cb->collapsedsize, cb->ncollapsed);
    cb->collapsed[cb->ncollapsed++] = dupstr(folder);
    save_collapsed(cb);
}

static int cmp_ci(const void *av, const void *bv)
{
    return _stricmp(*(const char *const *)av, *(const char *const *)bv);
}

/* Every folder, sorted: the ones made to put sessions in, the ones
 * sessions are in, and the ones those are in */
static char **all_folders(ConfigBox *cb, size_t *n)
{
    char **folders = NULL;
    size_t nfolders = 0, foldersize = 0;
    for (size_t i = 0; i < cb->nsess + cb->nfolders; i++) {
        /* (a folder made to put sessions in is as if it had a session
         * called "" in it, as in add_tree) */
        char *name = i >= cb->nsess ?
            dupcat(cb->folders[i - cb->nsess], FOLDER_SEP_S) :
            dupstr(cb->sess[i].name);
        for (char *slash = strchr(name, FOLDER_SEP); slash;
             slash = strchr(slash + 1, FOLDER_SEP)) {
            char *folder = dupprintf("%.*s", (int)(slash - name), name);
            bool seen = !*folder;
            for (size_t j = 0; j < nfolders && !seen; j++)
                seen = !strcmp(folders[j], folder);
            if (seen) {
                sfree(folder);
                continue;
            }
            sgrowarray(folders, foldersize, nfolders);
            folders[nfolders++] = folder;
        }
        sfree(name);
    }
    if (nfolders)
        qsort(folders, nfolders, sizeof(*folders), cmp_ci);
    *n = nfolders;
    return folders;
}

/* The sessions whose names start with 'prefix' (a folder's path, with
 * its '#', or ""), as a tree: the folders in it first, then the
 * sessions */
static void add_tree(ConfigBox *cb, const char *prefix, int indent)
{
    size_t plen = strlen(prefix);
    char **folders = NULL;
    size_t nfolders = 0, foldersize = 0;
    for (size_t i = 0; i < cb->nsess + cb->nfolders; i++) {
        /* (the folders sessions are in, and the ones made to put them
         * in, which are as if they had a session called "") */
        char *made = i >= cb->nsess ?
            dupcat(cb->folders[i - cb->nsess], FOLDER_SEP_S) : NULL;
        const char *name = made ? made : cb->sess[i].name;
        if (strncmp(name, prefix, plen)) {
            sfree(made);
            continue;
        }
        const char *slash = strchr(name + plen, FOLDER_SEP);
        if (!slash || slash == name + plen) {
            sfree(made);
            continue;
        }
        char *folder = dupprintf("%.*s", (int)(slash - name), name);
        sfree(made);
        bool seen = false;
        for (size_t j = 0; j < nfolders && !seen; j++)
            seen = !strcmp(folders[j], folder);
        if (seen) {
            sfree(folder);
            continue;
        }
        sgrowarray(folders, foldersize, nfolders);
        folders[nfolders++] = folder;
    }
    qsort(folders, nfolders, sizeof(*folders), cmp_ci);

    for (size_t f = 0; f < nfolders; f++) {
        char *path = dupcat(folders[f], FOLDER_SEP_S);
        size_t count = 0;
        for (size_t i = 0; i < cb->nsess; i++)
            count += !strncmp(cb->sess[i].name, path, strlen(path));
        bool open = !is_collapsed(cb, folders[f]);
        int id = add_rowkey(cb, 'F', folders[f]);
        wchar_t *title = to_wide(folders[f] + plen, false);
        wchar_t detail[32];
        if (!count)
            wcscpy(detail, L"Empty");
        else
            swprintf(detail, lenof(detail), count == 1 ? L"%u session" :
                     L"%u sessions", (unsigned)count);
        wui_w_rows_add(cb->rows, id, WUI_ROW_FOLDER, indent,
                       open ? 0xE838 : 0xE8B7,   /* FolderOpen, Folder */
                       title, NULL, detail, false);
        sfree(title);
        if (open)
            add_tree(cb, path, indent + 1);
        sfree(path);
        sfree(folders[f]);
    }
    sfree(folders);

    for (size_t i = 0; i < cb->nsess; i++) {
        const char *name = cb->sess[i].name;
        if (!strncmp(name, prefix, plen) &&
            (!strchr(name + plen, FOLDER_SEP) || name[plen] == FOLDER_SEP))
            add_session_row(cb, &cb->sess[i], NAME_IN_TREE, indent);
    }
}

static bool contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (!_strnicmp(hay, needle, n))
            return true;
    return !*needle;
}

static char *search_text(ConfigBox *cb)
{
    char *text = cb->search ? widget_text(cb->search, false) : dupstr("");
    char *p = text + strspn(text, " \t");
    size_t len = strlen(p);
    while (len && (p[len - 1] == ' ' || p[len - 1] == '\t'))
        len--;
    char *ret = dupprintf("%.*s", (int)len, p);
    sfree(text);
    return ret;
}

/* A section's heading: clicking it shows or hides what's
 * under it (and says how many there are while they're hidden) */
static bool add_heading(ConfigBox *cb, unsigned section,
                        const wchar_t *title, size_t count)
{
    char k[2] = { "0123456789"[section], '\0' };
    bool shown = !(cb->hidden & section);
    wchar_t detail[32];
    if (shown)
        detail[0] = L'\0';
    else
        swprintf(detail, lenof(detail), count == 1 ? L"%u session" :
                 L"%u sessions", (unsigned)count);
    wui_w_rows_add(cb->rows, add_rowkey(cb, 'H', k), WUI_ROW_HEADING, 0,
                   shown ? 0xE70D : 0xE76C,   /* ChevronDown, ChevronRight */
                   title, NULL, detail, false);
    return shown;
}

static int cmp_recent(const void *av, const void *bv)
{
    const SessionInfo *a = *(const SessionInfo *const *)av;
    const SessionInfo *b = *(const SessionInfo *const *)bv;
    return a->last_used < b->last_used ? 1 : a->last_used > b->last_used ?
        -1 : 0;
}

/* Fill in the rows, keeping the row with key 'keep' selected (or if
 * that's NULL, whichever one is now) */
static void refresh_rows(ConfigBox *cb, const char *keep)
{
    if (!cb->rows)
        return;
    char *sel = NULL;
    if (keep) {
        sel = dupstr(keep);
    } else {
        int id = wui_w_rows_selected(cb->rows);
        if (id >= 0 && (size_t)id < cb->nrowkeys)
            sel = dupstr(cb->rowkeys[id]);
    }
    clear_rowkeys(cb);
    cb->tree_rows = SIZE_MAX;
    wui_w_rows_clear(cb->rows);

    char *q = search_text(cb);
    /* (things can be moved by dragging them about in the tree, but
     * not in a list of what matches) */
    wui_w_rows_draggable(cb->rows, !*q);
    if (*q) {
        /* What matches, the ones whose names start with it first; and
         * connecting to it as a host name (first, if nothing matches) */
        SessionInfo **m = snewn(cb->nsess + 1, SessionInfo *);
        size_t nm = 0;
        /* (with the names as shown: "Clients/web" finds the session
         * "web" in the folder "Clients") */
        for (int pass = 0; pass < 2; pass++)
            for (size_t i = 0; i < cb->nsess; i++) {
                SessionInfo *si = &cb->sess[i];
                char *shown = name_shown(si->name);
                bool starts = !_strnicmp(shown, q, strlen(q));
                if (pass == 0 ? starts :
                    !starts && (contains_ci(shown, q) ||
                                contains_ci(si->host, q) ||
                                contains_ci(si->user, q)))
                    m[nm++] = si;
                sfree(shown);
            }
        bool hostlike = !strpbrk(q, " \t");
        wchar_t *wq = to_wide(q, false);
        wchar_t *title = dupwcscat(L"Connect to ", wq);
        wchar_t *how = quick_how(q);
        if (hostlike && !nm)
            wui_w_rows_add(cb->rows, add_rowkey(cb, 'Q', q), WUI_ROW_LINK,
                           0, 0xE703, title, how, NULL, false);
        for (size_t i = 0; i < nm; i++)
            add_session_row(cb, m[i], NAME_FULL, 0);
        if (hostlike && nm)
            wui_w_rows_add(cb->rows, add_rowkey(cb, 'Q', q), WUI_ROW_LINK,
                           0, 0xE703, title, how, NULL, false);
        sfree(how);
        sfree(title);
        sfree(wq);
        sfree(m);
    } else if (cb->nsess || cb->nfolders) {
        bool headings = false;
        SessionInfo **list = snewn(cb->nsess, SessionInfo *);
        size_t n = 0;
        for (size_t i = 0; i < cb->nsess; i++)
            if (cb->sess[i].favorite)
                list[n++] = &cb->sess[i];
        if (n) {
            if (add_heading(cb, SECTION_FAVORITES, L"Favorites", n))
                for (size_t i = 0; i < n; i++)
                    add_session_row(cb, list[i], NAME_IN_SECTION, 0);
            headings = true;
        }
        n = 0;
        for (size_t i = 0; i < cb->nsess; i++)
            if (cb->sess[i].last_used && !cb->sess[i].favorite)
                list[n++] = &cb->sess[i];
        qsort(list, n, sizeof(*list), cmp_recent);
        if (n > 5)
            n = 5;
        if (n) {
            if (add_heading(cb, SECTION_RECENT, L"Recent", n))
                for (size_t i = 0; i < n; i++)
                    add_session_row(cb, list[i], NAME_IN_SECTION, 0);
            headings = true;
        }
        sfree(list);
        /* (and if there's nothing else, just the tree, with no heading
         * to hide it with) */
        if (!headings || add_heading(cb, SECTION_ALL, L"All sessions",
                                     cb->nsess)) {
            cb->tree_rows = cb->nrowkeys;
            add_tree(cb, "", 0);
        }
    }
    sfree(q);

    if (sel) {
        for (size_t i = 0; i < cb->nrowkeys; i++)
            if (!strcmp(cb->rowkeys[i], sel)) {
                wui_w_rows_select(cb->rows, (int)i);
                break;
            }
        sfree(sel);
    }
}

static void build_home(ConfigBox *cb)
{
    WuiDlg *dlg = cb->dlg;
    static const wchar_t *const crumbs[1] = { L"Sessions" };
    static const int ids[1] = { HOME_ID };
    wui_dlg_page_title(dlg, 1, crumbs, ids, L"Connect to a saved session, "
                       L"or type a host name to connect to.");

    wui_dlg_page_button(dlg, ACT(cb, ACT_NEW), L"New session", 0xE710, false);
    WuiWidget *more = wui_dlg_page_menu(dlg, L"More");
    /* (at the top level; the sessions' and folders' menus have it too,
     * for in the folder they're in) */
    wui_w_menu_add(more, ACT(cb, ACT_NEW_FOLDER), L"New folder...", 0xE8F4);
    wui_w_menu_add(more, NULL, NULL, 0);
    wui_w_menu_add(more, ACT(cb, ACT_IMPORT), L"Import sessions...", 0xE8B5);
    wui_w_menu_add(more, ACT(cb, ACT_EXPORT_ALL), L"Export all sessions...",
                   0xEDE1);
    wui_w_menu_add(more, NULL, NULL, 0);
    wui_w_menu_add(more, ACT(cb, ACT_DEFAULTS), L"Defaults for new sessions",
                   0xE713);

    WuiWidget *g = wui_dlg_group(dlg, false, NULL);
    cb->search = wui_w_search(g, ACT(cb, ACT_SEARCH),
                              L"Host name to connect to (user@host:port, "
                              L"telnet://host...), or a session to look for",
                              L"Connect");

    g = wui_dlg_group(dlg, false, NULL);
    wui_dlg_group_bare(g);
    cb->rows = wui_w_rows(g, ACT(cb, ACT_ROWS), L"There are no saved "
                          L"sessions yet. Type a host name above to connect "
                          L"to it, or make a New session.");
    WuiWidget *r = cb->rows;
    wui_w_rows_action(r, L"Connect", NULL, 0xE768, 0, ACT(cb, ACT_CONNECT),
                      0);                          /* Play */
    wui_w_rows_action(r, L"Connect in a new window", NULL, 0xE8A7, 0,
                      ACT(cb, ACT_NEWWIN), 0);     /* OpenInNewWindow */
    wui_w_rows_action(r, NULL, NULL, 0, 0, NULL, 0);
    wui_w_rows_action(r, L"Edit", NULL, 0xE70F, 0, ACT(cb, ACT_EDIT), 0);
    wui_w_rows_action(r, L"Duplicate...", NULL, 0xE8C8, 0,
                      ACT(cb, ACT_DUPLICATE), 0);
    wui_w_rows_action(r, L"Rename...", NULL, 0xE8AC, 0, ACT(cb, ACT_RENAME),
                      VK_F2);
    wui_w_rows_action(r, L"Add to favorites", L"Remove from favorites",
                      0xE734, 0xE8D9, ACT(cb, ACT_FAVORITE), 0);
                                        /* FavoriteStar, Unfavorite */
    wui_w_rows_action(r, L"New folder...", NULL, 0xE8F4, 0,
                      ACT(cb, ACT_NEW_FOLDER_BESIDE), 0);  /* NewFolder */
    wui_w_rows_action(r, NULL, NULL, 0, 0, NULL, 0);
    wui_w_rows_action(r, L"Copy command line", NULL, 0xE943, 0,
                      ACT(cb, ACT_COPYCMD), 0);    /* Code */
    wui_w_rows_action(r, L"Export...", NULL, 0xEDE1, 0, ACT(cb, ACT_EXPORT),
                      0);
    wui_w_rows_action(r, NULL, NULL, 0, 0, NULL, 0);
    wui_w_rows_action(r, L"Delete", NULL, 0xE74D, 0, ACT(cb, ACT_DELETE),
                      VK_DELETE);
    wui_w_rows_folder_action(r, L"New session in this folder", 0xE710,
                             ACT(cb, ACT_NEW_HERE), 0);
    wui_w_rows_folder_action(r, L"New folder in this folder...", 0xE8F4,
                             ACT(cb, ACT_NEW_SUBFOLDER), 0); /* NewFolder */
    wui_w_rows_folder_action(r, NULL, 0, NULL, 0);
    wui_w_rows_folder_action(r, L"Rename...", 0xE8AC,
                             ACT(cb, ACT_RENAME_FOLDER), VK_F2);
    wui_w_rows_folder_action(r, L"Delete...", 0xE74D,
                             ACT(cb, ACT_DELETE_FOLDER), VK_DELETE);
    wui_w_rows_list_action(r, L"New session", 0xE710, ACT(cb, ACT_NEW));
    wui_w_rows_list_action(r, L"New folder...", 0xE8F4,
                           ACT(cb, ACT_NEW_FOLDER));
    wui_w_search_next(cb->search, cb->rows);

    reload_sessions(cb);
    refresh_rows(cb, NULL);
    wui_w_focus(cb->search);
}

static bool run_config_box(Conf *conf, HWND owner, bool midsession,
                           int protocol, int protcfginfo,
                           const char *session, char **launched,
                           bool defaults, bool destination);
static void free_box(ConfigBox *cb);

/* Mid-session, Apply: the main button only when there's no saved
 * session for Apply and save to save to */
static void update_apply(ConfigBox *cb)
{
    if (!cb->apply_w)
        return;
    wui_w_set_accent(cb->apply_w, !cb->session);
    wui_w_set_tooltip(cb->apply_w, cb->session ?
                      L"Apply the changes to this session only, without "
                      L"saving them" : L"Apply the changes to this session");
}

/* The buttons beside a settings page's title */
static void build_page_actions(ConfigBox *cb)
{
    WuiDlg *dlg = cb->dlg;
    cb->save_w = NULL;
    if (cb->midsession) {
        WuiWidget *more = NULL;
        if (cb->session) {
            /* (the session's name, which can be long, in the tooltip:
             * it's above the title already, and in the button it can
             * leave the title no room) */
            wchar_t *name = wide_shown(cb->session);
            wchar_t *tip = dupwcscat(L"Apply the changes, and save them "
                                     L"to ", name);
            /* (the main button, when there's a saved session to keep the
             * changes in: Apply, beside it, just tries them out) */
            WuiWidget *b = wui_dlg_page_button(
                dlg, ACT(cb, ACT_APPLY_SAVE), L"Apply and save", 0xE74E,
                true);
            wui_w_set_tooltip(b, tip);
            sfree(tip);
            sfree(name);
        }
        /* (the same menu as for editing a saved session) */
        more = wui_dlg_page_menu(dlg, L"More");
        if (cb->session) {
            wui_w_menu_add(more, ACT(cb, ACT_SAVE_AS), L"Save as a copy...",
                           0xE792);               /* SaveAs */
            wui_w_menu_add(more, ACT(cb, ACT_RENAME_TARGET), L"Rename...",
                           0xE8AC);
            wui_w_menu_add(more, ACT(cb, ACT_DELETE_TARGET), L"Delete",
                           0xE74D);
        } else {
            wui_w_menu_add(more, ACT(cb, ACT_SAVE_AS),
                           L"Save as a new session...", 0xE792);
        }
        wui_w_menu_add(more, NULL, NULL, 0);
        wui_w_menu_add(more, ACT(cb, ACT_DISCARD), L"Discard changes",
                       0xE7A7);
        wui_w_menu_add(more, NULL, NULL, 0);
        wui_w_menu_add(more, ACT(cb, ACT_DEFAULTS),
                       L"Defaults for new sessions", 0xE713);
        return;
    }

    if (cb->target == TARGET_NEW) {
        wui_dlg_page_button(dlg, ACT(cb, ACT_SAVE_AS), L"Save as...",
                            0xE74E, false);       /* Save */
    } else {
        cb->save_w = wui_dlg_page_button(dlg, ACT(cb, ACT_SAVE), L"Save",
                                         0xE74E, false);
    }
    WuiWidget *more = wui_dlg_page_menu(dlg, L"More");
    if (cb->target == TARGET_SESSION) {
        wui_w_menu_add(more, ACT(cb, ACT_SAVE_AS), L"Save as a copy...",
                       0xE792);                   /* SaveAs */
        wui_w_menu_add(more, ACT(cb, ACT_RENAME_TARGET), L"Rename...",
                       0xE8AC);
        wui_w_menu_add(more, ACT(cb, ACT_DELETE_TARGET), L"Delete", 0xE74D);
        wui_w_menu_add(more, NULL, NULL, 0);
    }
    wui_w_menu_add(more, ACT(cb, ACT_DISCARD), cb->target == TARGET_NEW ?
                   L"Start again from the defaults" : L"Discard changes",
                   0xE7A7);
    if (cb->target != TARGET_DEFAULTS) {
        wui_w_menu_add(more, NULL, NULL, 0);
        wui_w_menu_add(more, ACT(cb, ACT_DEFAULTS),
                       L"Defaults for new sessions", 0xE713);
    }
}

/* ---- Doing things with sessions ---- */

/* Is ask_name asking for just a session's own name, without its
 * folder? */
static bool is_leaf_prompt(int kind)
{
    return kind == PROMPT_SAVE_AS || kind == PROMPT_RENAME ||
        kind == PROMPT_RENAME_TARGET || kind == PROMPT_DUPLICATE;
}

static void ask_name(ConfigBox *cb, int kind, const char *about,
                     const wchar_t *title, const wchar_t *text,
                     const char *initial, const wchar_t *confirm)
{
    cb->prompt_kind = kind;
    sfree(cb->prompt_for);
    cb->prompt_for = about ? dupstr(about) : NULL;
    wchar_t *winit = to_wide(initial, false);
    /* (a session's name can't have a folder in it: see prompted) */
    wui_dlg_prompt(cb->dlg, ACT(cb, ACT_PROMPTED), title, text, winit,
                   is_leaf_prompt(kind) || kind == PROMPT_NEW_FOLDER ||
                   kind == PROMPT_RENAME_FOLDER ? NAME_REFUSED_W : NULL,
                   confirm);
    sfree(winit);
}

static void forget_undo(ConfigBox *cb)
{
    if (cb->undo_conf)
        conf_free(cb->undo_conf);
    cb->undo_conf = NULL;
    sfree(cb->undo_name);
    cb->undo_name = NULL;
}

static void delete_session(ConfigBox *cb, const char *name)
{
    /* (keeping it, and its hidden commands, for Undo) */
    forget_undo(cb);
    cb->undo_conf = conf_new();
    if (!session_load_plain(name, cb->undo_conf)) {
        forget_undo(cb);
        return;
    }
    cb->undo_name = dupstr(name);
    SessionInfo *si = find_session(cb, name);
    cb->undo_favorite = si && si->favorite;
    cb->undo_last_used = si ? si->last_used : 0;
    session_delete(name);

    if (cb->target == TARGET_SESSION && cb->target_name &&
        !strcmp(cb->target_name, name)) {
        /* (what was being edited has gone, so start a new one) */
        set_target(cb, TARGET_NEW, NULL);
    }

    reload_sessions(cb);
    if (cb->current != HOME_ID) {
        cb->current = -1;
        show_category(cb, HOME_ID);
    } else {
        refresh_rows(cb, NULL);
    }
    wchar_t *wname = wide_shown(name);
    wchar_t *msg = dupwcscat(L"Deleted ", wname);
    wui_dlg_notice(cb->dlg, msg, L"Undo", ACT(cb, ACT_UNDO), 15, false);
    sfree(msg);
    sfree(wname);
}

/*
 * Mid-session, the saved session's name as typed on the Session page,
 * once the box is left (or the dialog's done with): only its own name,
 * as for Rename, so it stays in its folder. 'quietly', when there's no
 * page left to say anything on.
 */
static void commit_session_name(ConfigBox *cb, bool quietly)
{
    if (!cb->name_w || !cb->session)
        return;
    size_t len = wui_w_get_text(cb->name_w, NULL, 0);
    wchar_t *wtext = snewn(len + 1, wchar_t);
    wui_w_get_text(cb->name_w, wtext, len + 1);
    char *raw = dup_wc_to_mb(CP_ACP, wtext, "");
    sfree(wtext);
    char *leaf = raw + strspn(raw, " \t");
    len = strlen(leaf);
    while (len && strchr(" \t", leaf[len - 1]))
        leaf[--len] = '\0';

    char *folder = parent_of(cb->session);
    char *full = name_in(folder, leaf);
    sfree(folder);
    char *err = NULL;
    /* (it can't have a folder separator in it: see wui_w_refuse_chars) */
    if (!*leaf) {
        if (!quietly)
            notice(cb, true, "A session needs a name");
    } else if (strcmp(full, cb->session) &&
               (quietly ? !session_exists(full) :
                name_ok(cb, full, cb->session)) &&
               (err = session_rename(cb->session, full)) == NULL) {
        char *from = dupstr(cb->session);
        reload_sessions(cb);
        if (quietly) {
            sfree(cb->session);
            cb->session = dupstr(full);
            cb->session_moved = true;
        } else {
            session_renamed(cb, from, full);
        }
        sfree(from);
    }
    if (err) {
        if (!quietly)
            notice(cb, true, "%s", err);
        sfree(err);
    }
    /* (and the box says what it's called now, if that didn't work) */
    if (!quietly && cb->name_w) {
        wchar_t *now = to_wide(leaf_of(cb->session), false);
        wui_w_set_text(cb->name_w, now);
        sfree(now);
    }
    sfree(full);
    sfree(raw);
}

/* Mid-session, Delete: the tab's saved session goes (with Undo), and
 * the tab goes on without one */
static void delete_tab_session(ConfigBox *cb)
{
    forget_undo(cb);
    cb->undo_conf = conf_new();
    if (!session_load_plain(cb->session, cb->undo_conf)) {
        forget_undo(cb);
        return;
    }
    cb->undo_name = dupstr(cb->session);
    SessionInfo *si = find_session(cb, cb->session);
    cb->undo_favorite = si && si->favorite;
    cb->undo_last_used = si ? si->last_used : 0;
    session_delete(cb->session);
    sfree(cb->session);
    cb->session = NULL;
    cb->session_moved = true;
    reload_sessions(cb);
    update_apply(cb);
    rebuild_page(cb);
    wchar_t *wname = wide_shown(cb->undo_name);
    wchar_t *msg = dupwcscat(L"Deleted ", wname);
    wui_dlg_notice(cb->dlg, msg, L"Undo", ACT(cb, ACT_UNDO), 15, false);
    sfree(msg);
    sfree(wname);
}

static void undo_delete(ConfigBox *cb)
{
    if (!cb->undo_conf)
        return;
    char *err = session_store(cb->undo_name, cb->undo_conf);
    if (err) {
        notice(cb, true, "%s", err);
        sfree(err);
    }
    session_set_favorite(cb->undo_name, cb->undo_favorite);
    session_set_last_used(cb->undo_name, cb->undo_last_used);
    if (cb->midsession && !cb->session) {
        /* (it's the tab's saved session again) */
        cb->session = dupstr(cb->undo_name);
        cb->session_moved = true;
        update_apply(cb);
        rebuild_page(cb);
    }
    char *key = dupcat("S", cb->undo_name);
    forget_undo(cb);
    reload_sessions(cb);
    refresh_rows(cb, key);
    sfree(key);
}

/* ---- Folders ---- */

/* The folder a session or folder is in ("" for the top level), and
 * its name in there */
static char *parent_of(const char *name)
{
    const char *slash = strrchr(name, FOLDER_SEP);
    return slash ? dupprintf("%.*s", (int)(slash - name), name) :
        dupstr("");
}

static const char *leaf_of(const char *name)
{
    const char *slash = strrchr(name, FOLDER_SEP);
    return slash ? slash + 1 : name;
}

static char *name_in(const char *folder, const char *leaf)
{
    return *folder ? dupcat(folder, FOLDER_SEP_S, leaf) : dupstr(leaf);
}

/* Is 'name' 'folder', or in it? */
static bool within(const char *name, const char *folder)
{
    size_t len = strlen(folder);
    return !strncmp(name, folder, len) &&
        (!name[len] || name[len] == FOLDER_SEP);
}

static size_t sessions_in(ConfigBox *cb, const char *folder)
{
    size_t n = 0;
    for (size_t i = 0; i < cb->nsess; i++)
        n += within(cb->sess[i].name, folder) &&
            strcmp(cb->sess[i].name, folder);
    return n;
}

/* A folder's own name as typed (it's typed in the folder it's to be
 * in): without spaces at either end, and not empty or with a folder
 * separator, or another character PuTTY-ng wouldn't have, in it */
static char *folder_leaf(const char *typed)
{
    const char *p = typed + strspn(typed, " \t");
    size_t len = strlen(p);
    while (len && strchr(" \t", p[len - 1]))
        len--;
    char *leaf = dupprintf("%.*s", (int)len, p);
    if (!*leaf || strpbrk(leaf, "#/\\")) {
        sfree(leaf);
        return NULL;
    }
    return leaf;
}

/* A session has a new name: if it's what's being edited, keep up */
static void session_renamed(ConfigBox *cb, const char *from, const char *to)
{
    if (cb->midsession && cb->session && !strcmp(cb->session, from)) {
        sfree(cb->session);
        cb->session = dupstr(to);
        cb->session_moved = true;
        rebuild_page(cb);              /* (the buttons have its name) */
        return;
    }
    if (cb->target == TARGET_SESSION && cb->target_name &&
        !strcmp(cb->target_name, from)) {
        sfree(cb->target_name);
        cb->target_name = dupstr(to);
        update_state(cb);
    }
}

static void show_row(ConfigBox *cb, char kind, const char *name)
{
    char k[2] = { kind, '\0' };
    char *key = dupcat(k, name);
    char *parent = parent_of(name);
    expand_folder(cb, parent);
    sfree(parent);
    reload_sessions(cb);
    refresh_rows(cb, key);
    sfree(key);
}

/* A new folder, in 'parent' ("" for the top level) */
static void new_folder(ConfigBox *cb, const char *parent, const char *typed)
{
    char *leaf = folder_leaf(typed);
    if (!leaf) {
        notice(cb, true, "\"%s\" can't be the name of a folder", typed);
        return;
    }
    char *path = name_in(parent, leaf);
    sfree(leaf);
    folder_add(path);
    show_row(cb, 'F', path);
    sfree(path);
}

/*
 * Move everything in a folder into another (perhaps "", the top
 * level), keeping what's there already: a session with the same name
 * as one already there stays where it is. Says how many did.
 */
static size_t move_folder(ConfigBox *cb, const char *from, const char *to)
{
    size_t len = strlen(from), stayed = 0;
    for (size_t i = 0; i < cb->nsess; i++) {
        const char *name = cb->sess[i].name;
        if (!within(name, from) || !name[len])
            continue;
        char *moved = name_in(to, name + len + 1);
        char *err = NULL;
        if (session_exists(moved))
            stayed++;
        else if ((err = session_rename(name, moved)) != NULL)
            stayed++, sfree(err);
        else
            session_renamed(cb, name, moved);
        sfree(moved);
    }
    folder_rename(from, to);
    if (*to)
        folder_add(to);

    /* (and what was open stays open) */
    for (size_t i = 0; i < cb->ncollapsed; i++) {
        if (!within(cb->collapsed[i], from))
            continue;
        const char *rest = cb->collapsed[i] + len;
        char *moved = *to ? dupcat(to, rest) :
            dupstr(rest + (*rest == FOLDER_SEP));
        sfree(cb->collapsed[i]);
        cb->collapsed[i] = moved;
    }
    save_collapsed(cb);
    return stayed;
}

static void moved_notice(ConfigBox *cb, size_t stayed)
{
    if (stayed)
        notice(cb, true, stayed == 1 ? "%u session wasn't moved, as there "
               "was one of the same name there already" : "%u sessions "
               "weren't moved, as there were ones of the same names there "
               "already", (unsigned)stayed);
}

/* (only its own name: it stays where it is, as a session does, and
 * moving it is for dragging it) */
static void rename_folder(ConfigBox *cb, const char *from, const char *typed)
{
    char *leaf = folder_leaf(typed);
    if (!leaf) {
        notice(cb, true, "\"%s\" can't be the name of a folder", typed);
        return;
    }
    char *parent = parent_of(from);
    char *to = name_in(parent, leaf);
    sfree(parent);
    sfree(leaf);
    if (strcmp(to, from) && within(to, from)) {
        notice(cb, true, "A folder can't go in itself");
    } else if (strcmp(to, from)) {
        size_t stayed = move_folder(cb, from, to);
        show_row(cb, 'F', to);
        moved_notice(cb, stayed);
    }
    sfree(to);
}

static void delete_folder(ConfigBox *cb, const char *path)
{
    size_t n = sessions_in(cb, path);
    if (n) {
        char *parent = parent_of(path);
        wchar_t *wpath = wide_shown(path);
        wchar_t *wparent = wide_shown(parent);
        wchar_t *main = dupwcscat(L"Delete the folder ", wpath, L"?");
        wchar_t text[256];
        swprintf(text, lenof(text), L"It has %u saved session%ls in it "
                 L"(counting the folders in it). They can be deleted too, "
                 L"or kept, and moved to %ls%ls.", (unsigned)n,
                 n == 1 ? L"" : L"s", *parent ? L"" : L"the top level",
                 wparent);
        int r = ask(cb, main, text, n == 1 ? L"Delete it too" :
                    L"Delete them too", n == 1 ? L"Keep it" : L"Keep them");
        sfree(main);
        sfree(wparent);
        sfree(wpath);
        if (r == 2) {
            size_t stayed = move_folder(cb, path, parent);
            folder_remove(path);
            reload_sessions(cb);
            refresh_rows(cb, NULL);
            moved_notice(cb, stayed);
        } else if (r == 1) {
            for (size_t i = 0; i < cb->nsess; i++) {
                const char *name = cb->sess[i].name;
                if (!within(name, path) || !strcmp(name, path))
                    continue;
                if (cb->target == TARGET_SESSION && cb->target_name &&
                    !strcmp(cb->target_name, name))
                    set_target(cb, TARGET_NEW, NULL);
                session_delete(name);
            }
            folder_remove(path);
            reload_sessions(cb);
            refresh_rows(cb, NULL);
            char *shown = name_shown(path);
            notice(cb, false, n == 1 ? "Deleted %s and the session in it" :
                   "Deleted %s and the %u sessions in it", shown,
                   (unsigned)n);
            sfree(shown);
        }
        sfree(parent);
        return;
    }
    folder_remove(path);
    reload_sessions(cb);
    refresh_rows(cb, NULL);
}

/* A row dragged onto another (or not onto one) */
static void dropped(ConfigBox *cb)
{
    int what, where;
    wui_w_rows_dropped(cb->rows, &what, &where);
    if (what < 0 || (size_t)what >= cb->nrowkeys ||
        (where >= 0 && (size_t)where >= cb->nrowkeys))
        return;
    /* (into a folder, or the one the session it's on is in) */
    char *to;
    const char *on = where >= 0 ? cb->rowkeys[where] : "F";
    if (on[0] == 'F')
        to = dupstr(on + 1);
    else if (on[0] == 'S')
        to = parent_of(on + 1);
    else
        return;
    char *key = dupstr(cb->rowkeys[what]);
    const char *name = key + 1;
    char *parent = parent_of(name);
    if (!strcmp(parent, to) || (key[0] != 'S' && key[0] != 'F')) {
        /* (it's there already) */
    } else if (key[0] == 'F' && within(to, name)) {
        notice(cb, true, "A folder can't go in itself");
    } else {
        char *moved = name_in(to, leaf_of(name));
        if (key[0] == 'S') {
            if (name_ok(cb, moved, NULL)) {
                char *err = session_rename(name, moved);
                if (err) {
                    notice(cb, true, "%s", err);
                    sfree(err);
                } else {
                    session_renamed(cb, name, moved);
                    show_row(cb, 'S', moved);
                }
            }
        } else {
            size_t stayed = move_folder(cb, name, moved);
            show_row(cb, 'F', moved);
            moved_notice(cb, stayed);
        }
        sfree(moved);
    }
    sfree(parent);
    sfree(key);
    sfree(to);
}

static wchar_t *pick_file(HWND owner, const char *title,
                          const wchar_t *initial, bool save,
                          FilereqFilter filter);

static void export_sessions(ConfigBox *cb, const char *name)
{
    char *initial = name ? dupcat(name, ".reg") : dupstr("PuTTY sessions.reg");
    for (char *p = initial; *p; p++)
        if (strchr("/\\:*?\"<>|", *p))
            *p = '_';
    wchar_t *winit = to_wide(initial, false);
    sfree(initial);
    wchar_t *file = pick_file(cb->dp.hwnd, name ? "Export Session" :
                              "Export Sessions", winit, true,
                              FILTER_ALL_FILES);
    sfree(winit);
    if (!file)
        return;
    char *err = sessions_export(name, file);
    sfree(file);
    if (err) {
        notice(cb, true, "Couldn't export: %s", err);
        sfree(err);
    } else {
        notice(cb, false, "Exported. (Hidden Automate logon commands stay "
               "in this computer's Credential Manager, so they aren't in "
               "the file.)");
    }
}

static void import_sessions(ConfigBox *cb)
{
    wchar_t *file = pick_file(cb->dp.hwnd, "Import Sessions", NULL, false,
                              FILTER_ALL_FILES);
    if (!file)
        return;
    int n = 0;
    char *err = sessions_import(file, &n);
    sfree(file);
    if (err) {
        notice(cb, true, "%s", err);
        sfree(err);
        return;
    }
    reload_sessions(cb);
    refresh_rows(cb, NULL);
    notice(cb, false, n == 1 ? "Imported %d session" :
           "Imported %d sessions", n);
}

/* A setting into every saved session */
static void apply_to_all(ConfigBox *cb, dlgcontrol *ctrl)
{
    int key = ctrl_conf_key(ctrl);
    if (key < 0)
        return;
    reload_sessions(cb);
    if (!cb->nsess) {
        notice(cb, true, "There are no saved sessions");
        return;
    }
    char *label = dupstr(ctrl->label ? ctrl->label : "this setting");
    size_t len = strlen(label);
    while (len && (label[len - 1] == ':' || label[len - 1] == ' '))
        label[--len] = '\0';
    char *text = dupprintf("\"%s\" will be set as it is here in all %u "
                           "saved sessions.", label, (unsigned)cb->nsess);
    wchar_t *wtext = to_wide(text, false);
    int r = ask(cb, L"Use this setting in all saved sessions?", wtext,
                L"Change them all", NULL);
    sfree(wtext);
    sfree(text);
    if (r != 1) {
        sfree(label);
        return;
    }

    Conf *conf = box_conf(cb);
    unsigned done = 0;
    for (size_t i = 0; i < cb->nsess; i++) {
        Conf *c = conf_new();
        if (load_settings(cb->sess[i].name, c)) {
            conf_key_copy(c, conf, key);
            char *err = save_settings(cb->sess[i].name, c);
            if (err)
                sfree(err);
            else
                done++;
        }
        conf_free(c);
    }
    /* (what's being edited has it saved now, as well) */
    if (cb->target == TARGET_SESSION)
        conf_key_copy(cb->saved, conf, key);
    update_state(cb);
    notice(cb, false, "Set \"%s\" in %u sessions", label, done);
    sfree(label);
}

/* The answer to one of ask_name's questions */
static void prompted(ConfigBox *cb)
{
    size_t len = wui_dlg_prompt_text(cb->dlg, NULL, 0);
    wchar_t *wname = snewn(len + 1, wchar_t);
    wui_dlg_prompt_text(cb->dlg, wname, len + 1);
    char *raw = dup_wc_to_mb(CP_ACP, wname, "");
    sfree(wname);
    char *name = raw + strspn(raw, " \t");
    len = strlen(name);
    while (len && (name[len - 1] == ' ' || name[len - 1] == '\t'))
        name[--len] = '\0';
    const char *about = cb->prompt_for;
    char *err = NULL;

    /* (a session's name is just its own: it goes in the folder of the
     * one it's made from, or the one chosen for Save as, and moving it
     * is for dragging it) */
    char *full = NULL;
    if (is_leaf_prompt(cb->prompt_kind)) {
        if (!about && cb->prompt_kind != PROMPT_SAVE_AS)
            goto done;
        if (strpbrk(name, "#/\\")) {
            notice(cb, true, "A session's name can't have '#', '/' or '\\' "
                   "in it");
            goto done;
        }
        char *folder;
        if (cb->prompt_kind == PROMPT_SAVE_AS) {
            size_t flen = wui_dlg_prompt_folder(cb->dlg, NULL, 0);
            wchar_t *wf = snewn(flen + 1, wchar_t);
            wui_dlg_prompt_folder(cb->dlg, wf, flen + 1);
            folder = dup_wc_to_mb(CP_ACP, wf, "");
            for (char *q = folder; *q; q++)
                if (*q == '/')
                    *q = FOLDER_SEP;
            sfree(wf);
        } else {
            folder = parent_of(about);
        }
        full = *name ? name_in(folder, name) : dupstr("");
        sfree(folder);
    }

    switch (cb->prompt_kind) {
      case PROMPT_SAVE_AS:
        if (!name_ok(cb, full, NULL))
            break;
        if ((err = session_store(full, box_conf(cb))) != NULL)
            break;
        reload_sessions(cb);
        if (cb->midsession) {
            char *shown = name_shown(full);
            notice(cb, false, "Saved as \"%s\"", shown);
            sfree(shown);
            break;
        }
        cb->target = TARGET_SESSION;
        sfree(cb->target_name);
        cb->target_name = dupstr(full);
        conf_copy_into(cb->saved, box_conf(cb));
        rebuild_page(cb);
        {
            char *shown = name_shown(full);
            notice(cb, false, "Saved as \"%s\"", shown);
            sfree(shown);
        }
        break;
      case PROMPT_RENAME:
      case PROMPT_RENAME_TARGET:
        if (!strcmp(full, about) || !name_ok(cb, full, about))
            break;
        if ((err = session_rename(about, full)) != NULL)
            break;
        session_renamed(cb, about, full);
        reload_sessions(cb);
        {
            char *key = dupcat("S", full);
            refresh_rows(cb, key);
            sfree(key);
        }
        break;
      case PROMPT_NEW_FOLDER:
        /* (in the folder it was asked for in, if any) */
        new_folder(cb, about ? about : "", name);
        break;
      case PROMPT_RENAME_FOLDER:
        if (about)
            rename_folder(cb, about, name);
        break;
      case PROMPT_DUPLICATE:
        if (!name_ok(cb, full, NULL))
            break;
        if ((err = session_copy(about, full)) != NULL)
            break;
        reload_sessions(cb);
        {
            char *key = dupcat("S", full);
            refresh_rows(cb, key);
            sfree(key);
        }
        break;
    }
    if (err) {
        notice(cb, true, "%s", err);
        sfree(err);
    }
  done:
    sfree(full);
    sfree(raw);
}

/* Activating the first row (pressing Return in the search box, or the
 * Connect button, on the Home page) */
static void home_go(ConfigBox *cb)
{
    char *q = search_text(cb);
    int id = -1;
    if (*q) {
        if (cb->nrowkeys)
            id = 0;
    } else if (cb->rows) {
        id = wui_w_rows_selected(cb->rows);
    }
    sfree(q);
    if (id < 0 || (size_t)id >= cb->nrowkeys) {
        notice(cb, true, "Type a host name to connect to, or pick a session");
        return;
    }
    char *key = dupstr(cb->rowkeys[id]);
    if (key[0] == 'S')
        connect_session(cb, key + 1);
    else if (key[0] == 'Q')
        quick_connect(cb, key + 1);
    else if (key[0] == 'F') {
        toggle_folder(cb, key + 1);
        refresh_rows(cb, key);
    }
    sfree(key);
}

static void act(ConfigBox *cb, int a, WuiWidgetEvent ev)
{
    const char *name;

    if (a == ACT_SEARCH) {
        if (ev == WUI_EV_VALCHANGE)
            refresh_rows(cb, NULL);
        else if (ev == WUI_EV_ACTION)
            home_go(cb);
        return;
    }
    if (a == ACT_ROWS && ev == WUI_EV_DROP) {
        dropped(cb);
        return;
    }
    /* (the widgets of our own that say more than that they've been
     * pressed: see below for the rest, which don't) */
    switch (a) {
      case ACT_SESSION_NAME:
        if (ev == WUI_EV_UNFOCUS)
            commit_session_name(cb, false);
        return;
      case ACT_KEYFILE:
        credentials_event(cb, cb->keyfile_w, CONF_keyfile, ev);
        return;
      case ACT_CERTFILE:
        credentials_event(cb, cb->certfile_w, CONF_detached_cert, ev);
        return;
    }
    if (ev != WUI_EV_ACTION)
        return;

    switch (a) {
      case ACT_ROWS: {
        int id = wui_w_rows_activated(cb->rows);
        if (id < 0 || (size_t)id >= cb->nrowkeys)
            break;
        char *key = dupstr(cb->rowkeys[id]);
        if (key[0] == 'H') {
            cb->hidden ^= key[1] - '0';
            home_hidden_set(cb->hidden);
            refresh_rows(cb, NULL);
        } else if (key[0] == 'F') {
            toggle_folder(cb, key + 1);
            refresh_rows(cb, key);
        } else if (key[0] == 'Q') {
            quick_connect(cb, key + 1);
        } else if ((size_t)id >= cb->tree_rows) {
            /* (in the tree, it's to be looked at, or changed: connecting
             * to it is on its menu, or under Favorites or Recent) */
            edit(cb, TARGET_SESSION, key + 1);
        } else {
            connect_session(cb, key + 1);
        }
        sfree(key);
        break;
      }
      case ACT_CONNECT:
        if ((name = row_session(cb, -1)) != NULL) {
            char *n = dupstr(name);
            connect_session(cb, n);
            sfree(n);
        }
        break;
      case ACT_NEWWIN:
        if ((name = row_session(cb, -1)) != NULL) {
            session_touch(name);
            wui_launch_session_window(name);
            reload_sessions(cb);
            refresh_rows(cb, NULL);
        }
        break;
      case ACT_EDIT:
        if ((name = row_session(cb, -1)) != NULL) {
            char *n = dupstr(name);
            edit(cb, TARGET_SESSION, n);
            sfree(n);
        }
        break;
      case ACT_DUPLICATE:
        if ((name = row_session(cb, -1)) != NULL) {
            char *initial = dupcat(leaf_of(name), " (copy)");
            ask_name(cb, PROMPT_DUPLICATE, name, L"Duplicate session",
                     L"The name for the copy:", initial, L"Duplicate");
            sfree(initial);
        }
        break;
      case ACT_RENAME:
        if ((name = row_session(cb, -1)) != NULL)
            ask_name(cb, PROMPT_RENAME, name, L"Rename session", NULL,
                     leaf_of(name), L"Rename");
        break;
      case ACT_FAVORITE:
        if ((name = row_session(cb, -1)) != NULL) {
            SessionInfo *si = find_session(cb, name);
            char *key = dupcat("S", name);
            session_set_favorite(name, !(si && si->favorite));
            reload_sessions(cb);
            refresh_rows(cb, key);
            sfree(key);
        }
        break;
      case ACT_COPYCMD:
        if ((name = row_session(cb, -1)) != NULL) {
            wchar_t exe[MAX_PATH];
            if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
                wcscpy(exe, L"jputty.exe");
            wchar_t *wname = to_wide(name, false);
            wchar_t *cmd = dupwcscat(L"\"", exe, L"\" -load \"", wname,
                                     L"\"");
            copy_to_clipboard(cb->dp.hwnd, cmd);
            sfree(cmd);
            sfree(wname);
            notice(cb, false, "Copied the command line to open \"%s\"",
                   name);
        }
        break;
      case ACT_EXPORT:
        if ((name = row_session(cb, -1)) != NULL) {
            char *n = dupstr(name);
            export_sessions(cb, n);
            sfree(n);
        }
        break;
      case ACT_DELETE:
        if ((name = row_session(cb, -1)) != NULL) {
            char *n = dupstr(name);
            delete_session(cb, n);
            sfree(n);
        }
        break;
      case ACT_NEW_FOLDER:
        ask_name(cb, PROMPT_NEW_FOLDER, NULL, L"New folder", NULL, "",
                 L"Make it");
        break;
      case ACT_NEW_FOLDER_BESIDE:
        /* (in the folder the session's in) */
        if ((name = row_session(cb, -1)) != NULL) {
            char *parent = parent_of(name);
            if (!*parent) {
                ask_name(cb, PROMPT_NEW_FOLDER, NULL, L"New folder", NULL,
                         "", L"Make it");
            } else {
                wchar_t *wshown = wide_shown(parent);
                wchar_t *text = dupwcscat(L"In ", wshown);
                ask_name(cb, PROMPT_NEW_FOLDER, parent, L"New folder", text,
                         "", L"Make it");
                sfree(text);
                sfree(wshown);
            }
            sfree(parent);
        }
        break;
      case ACT_NEW_SUBFOLDER:
        if ((name = row_folder(cb, -1)) != NULL) {
            char *shown = name_shown(name);
            wchar_t *wshown = to_wide(shown, false);
            wchar_t *text = dupwcscat(L"In ", wshown);
            ask_name(cb, PROMPT_NEW_FOLDER, name, L"New folder", text, "",
                     L"Make it");
            sfree(text);
            sfree(wshown);
            sfree(shown);
        }
        break;
      case ACT_NEW_HERE:
        if ((name = row_folder(cb, -1)) != NULL) {
            char *folder = dupstr(name);
            edit(cb, TARGET_NEW, NULL);
            /* (if it went ahead, it's saved in there to begin with) */
            if (cb->target == TARGET_NEW && cb->current != HOME_ID) {
                cb->new_folder = folder;
                char *shown = name_shown(folder);
                notice(cb, false, "It'll be saved in %s", shown);
                sfree(shown);
            } else {
                sfree(folder);
            }
        }
        break;
      case ACT_RENAME_FOLDER:
        if ((name = row_folder(cb, -1)) != NULL)
            ask_name(cb, PROMPT_RENAME_FOLDER, name, L"Rename folder", NULL,
                     leaf_of(name), L"Rename");
        break;
      case ACT_DELETE_FOLDER:
        if ((name = row_folder(cb, -1)) != NULL) {
            char *n = dupstr(name);
            delete_folder(cb, n);
            sfree(n);
        }
        break;
      case ACT_NEW:
        edit(cb, TARGET_NEW, NULL);
        break;
      case ACT_IMPORT:
        import_sessions(cb);
        break;
      case ACT_EXPORT_ALL:
        export_sessions(cb, NULL);
        break;
      case ACT_DEFAULTS:
        if (cb->midsession) {
            /* (in a box of their own, over this one, as this one's
             * for the session) */
            Conf *c = conf_new();
            run_config_box(c, cb->dp.hwnd, false, 0, 0, NULL, NULL, true,
                           false);
            conf_free(c);
        } else if (cb->target == TARGET_DEFAULTS) {
            cb->current = -1;
            show_category(cb, session_page(cb));
        } else {
            edit(cb, TARGET_DEFAULTS, NULL);
        }
        break;
      case ACT_UNDO:
        undo_delete(cb);
        break;
      case ACT_SAVE:
        save_target(cb);
        break;
      case ACT_SAVE_AS: {
        /* (in the folder of the session it's a copy of, or the one a
         * new session was made in) */
        char *folder = cb->target_name ? parent_of(cb->target_name) :
            dupstr(cb->target == TARGET_NEW && cb->new_folder ?
                   cb->new_folder : "");
        size_t nf;
        char **folders = all_folders(cb, &nf);
        wchar_t **items = snewn(nf + 1, wchar_t *);
        size_t sel = 0;
        items[0] = dupwcs(L"(Top level)");
        for (size_t i = 0; i < nf; i++) {
            /* (shown, and given back, with '/' between the folders,
             * which no folder's name has in it: see prompted) */
            char *shown = name_shown(folders[i]);
            items[i + 1] = to_wide(shown, false);
            sfree(shown);
            if (!strcmp(folders[i], folder))
                sel = i + 1;
        }
        wui_dlg_prompt_choices(cb->dlg, L"Destination folder",
                               (const wchar_t *const *)items,
                               nf + 1, sel);
        for (size_t i = 0; i <= nf; i++)
            sfree(items[i]);
        sfree(items);
        folders_free(folders, nf);
        sfree(folder);
        ask_name(cb, PROMPT_SAVE_AS, NULL, cb->midsession ?
                 L"Save as a new session" : cb->target == TARGET_NEW ?
                 L"Save session" : L"Save as a copy", NULL,
                 cb->target_name ? leaf_of(cb->target_name) : "", L"Save");
        break;
      }
      case ACT_RENAME_TARGET: {
        const char *n = cb->midsession ? cb->session :
            cb->target == TARGET_SESSION ? cb->target_name : NULL;
        if (n)
            ask_name(cb, PROMPT_RENAME_TARGET, n, L"Rename session", NULL,
                     leaf_of(n), L"Rename");
        break;
      }
      case ACT_DELETE_TARGET:
        if (cb->midsession && cb->session) {
            delete_tab_session(cb);
        } else if (cb->target == TARGET_SESSION && cb->target_name) {
            char *n = dupstr(cb->target_name);
            /* (its unsaved changes go too, so no need to ask) */
            delete_session(cb, n);
            sfree(n);
        }
        break;
      case ACT_DISCARD:
        if (cb->midsession) {
            /* (back to the session's settings as they were when the
             * box opened) */
            conf_copy_into(box_conf(cb), cb->saved);
            dlg_auto_set_fixed_pitch_flag(&cb->dp);
        } else {
            set_target(cb, cb->target, cb->target_name);
        }
        rebuild_page(cb);
        break;
      case ACT_APPLY_SAVE:
        if (cb->session) {
            char *err = session_store(cb->session, box_conf(cb));
            if (err) {
                notice(cb, true, "%s", err);
                sfree(err);
            } else {
                dlg_end(&cb->dp, 1);
                wui_dlg_stop(cb->dlg);
            }
        }
        break;
      case ACT_PROMPTED:
        prompted(cb);
        break;
    }
}

/* Connect (or Apply): a few things to do before config.c's handler
 * for it goes ahead (if this returns true) */
static bool before_connect(ConfigBox *cb)
{
    if (cb->midsession)
        return true;
    if (cb->current == HOME_ID) {
        home_go(cb);
        return false;
    }
    if (!conf_launchable(box_conf(cb))) {
        notice(cb, true, "There's no host name to connect to: type one on "
               "the Address page");
        return false;
    }
    if (cb->dirty && cb->target != TARGET_NEW && !check_dirty(cb))
        return false;
    sfree(cb->launched);
    cb->launched = cb->target == TARGET_SESSION ?
        dupstr(cb->target_name) : NULL;
    session_touch(cb->launched);
    return true;
}

/* ----------------------------------------------------------------------
 * Events.
 */

void wui_dlg_cb_category(void *ctx, int id)
{
    ConfigBox *cb = (ConfigBox *)ctx;
    show_category(cb, id);
}

void wui_dlg_cb_close(void *ctx)
{
    ConfigBox *cb = (ConfigBox *)ctx;
    if (!cb->midsession && cb->dirty && cb->target != TARGET_NEW &&
        !check_dirty(cb))
        return;
    dlg_end(&cb->dp, 0);
    wui_dlg_stop(cb->dlg);
}

/*
 * Asking for a file: with Windows' own file dialog (not the one
 * request_file uses, which, without OFN_EXPLORER, is the one from
 * Windows 3.1). Returns the file's path to free, or NULL.
 */
static wchar_t *pick_file(HWND owner, const char *title,
                          const wchar_t *initial, bool save,
                          FilereqFilter filter)
{
    static const CLSID clsid_FileOpenDialog = {
        0xDC1C5A9C, 0xE88A, 0x4DDE,
        { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
    static const CLSID clsid_FileSaveDialog = {
        0xC0B4E2F3, 0xBA21, 0x4773,
        { 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B } };
    static const IID iid_IFileDialog = {
        0x42F85136, 0xDB7E, 0x439C,
        { 0x85, 0xF1, 0xE4, 0x07, 0x5D, 0x13, 0x5F, 0xC8 } };
    static const IID iid_IShellItem = {
        0x43826D1E, 0xE718, 0x42EE,
        { 0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE } };

    IFileDialog *dlg;
    if (FAILED(CoCreateInstance(save ? &clsid_FileSaveDialog :
                                &clsid_FileOpenDialog, NULL,
                                CLSCTX_INPROC_SERVER, &iid_IFileDialog,
                                (void **)&dlg)))
        return NULL;

    FILEOPENDIALOGOPTIONS opts;
    if (SUCCEEDED(dlg->lpVtbl->GetOptions(dlg, &opts)))
        dlg->lpVtbl->SetOptions(dlg, opts | FOS_FORCEFILESYSTEM |
                                FOS_NOCHANGEDIR | (save ? FOS_OVERWRITEPROMPT :
                                                   FOS_FILEMUSTEXIST));
    if (title) {
        wchar_t *wtitle = to_wide(title, false);
        dlg->lpVtbl->SetTitle(dlg, wtitle);
        sfree(wtitle);
    }

    /* (the kinds of file request_file offers, for the same purposes) */
    COMDLG_FILTERSPEC types[2];
    UINT ntypes = 0;
    const wchar_t *ext = NULL;
    switch (filter) {
      case FILTER_KEY_FILES:
        types[ntypes++] = (COMDLG_FILTERSPEC){
            L"PuTTY Private Key Files (*.ppk)", L"*.ppk" };
        ext = L"ppk";
        break;
      case FILTER_DYNLIB_FILES:
        types[ntypes++] = (COMDLG_FILTERSPEC){
            L"Dynamic Library Files (*.dll)", L"*.dll" };
        ext = L"dll";
        break;
      case FILTER_SOUND_FILES:
        types[ntypes++] = (COMDLG_FILTERSPEC){
            L"Wave Files (*.wav)", L"*.wav" };
        ext = L"wav";
        break;
      default:
        break;
    }
    types[ntypes++] = (COMDLG_FILTERSPEC){ L"All Files (*.*)", L"*" };
    dlg->lpVtbl->SetFileTypes(dlg, ntypes, types);
    if (ext)
        dlg->lpVtbl->SetDefaultExtension(dlg, ext);

    /* (starting where the file there already is, if any) */
    if (initial && *initial) {
        wchar_t *dir = dupwcs(initial);
        wchar_t *slash = wcsrchr(dir, L'\\');
        const wchar_t *leaf = slash ? initial + (slash - dir) + 1 : initial;
        if (slash) {
            *slash = L'\0';
            IShellItem *folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                              dir, NULL, &iid_IShellItem, (void **)&folder))) {
                dlg->lpVtbl->SetFolder(dlg, folder);
                folder->lpVtbl->Release(folder);
            }
        }
        dlg->lpVtbl->SetFileName(dlg, leaf);
        sfree(dir);
    }

    wchar_t *result = NULL;
    if (SUCCEEDED(dlg->lpVtbl->Show(dlg, owner))) {
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

/* A file selector's Browse button */
static void file_browse(ConfigBox *cb, dlgcontrol *ctrl)
{
    dlgparam *dp = &cb->dp;
    ConfigCtrl *c = find_ctrl(cb, ctrl);
    wchar_t *prev = NULL;

    if (!ctrl->fileselect.just_button) {
        size_t len = wui_w_get_text(c->w, NULL, 0);
        prev = snewn(len + 1, wchar_t);
        wui_w_get_text(c->w, prev, len + 1);
    }

    wchar_t *picked = pick_file(dp->hwnd, ctrl->fileselect.title, prev,
                                ctrl->fileselect.for_writing,
                                ctrl->fileselect.filter);
    sfree(prev);
    if (!picked)
        return;
    Filename *fn = filename_from_wstr(picked);
    sfree(picked);

    c = find_ctrl(cb, ctrl);           /* (in case it moved) */
    if (!ctrl->fileselect.just_button) {
        wui_w_set_text(c->w, filename_to_wstr(fn));
        ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
    } else {
        c->fn = fn;
        ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
        if ((c = find_ctrl(cb, ctrl)) != NULL)
            c->fn = NULL;
    }
    filename_free(fn);
}

/* A font selector's Change button */
/* The font families there are, for the font dialog: each once, not
 * the vertical ones ("@..."), and if 'fixed', only those whose
 * characters are all the same width */
struct font_list {
    wchar_t **names;
    size_t n, size;
    bool fixed;
};

static int CALLBACK font_listed(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                DWORD type, LPARAM lp)
{
    struct font_list *fl = (struct font_list *)lp;
    if (lf->lfFaceName[0] == L'@')
        return 1;
    if (fl->fixed && (lf->lfPitchAndFamily & 3) != FIXED_PITCH)
        return 1;
    for (size_t i = 0; i < fl->n; i++)
        if (!wcscmp(fl->names[i], lf->lfFaceName))
            return 1;
    sgrowarray(fl->names, fl->size, fl->n);
    fl->names[fl->n++] = dupwcs(lf->lfFaceName);
    return 1;
}

static int cmp_wide_ci(const void *a, const void *b)
{
    return _wcsicmp(*(const wchar_t *const *)a, *(const wchar_t *const *)b);
}

/* A font selector's Change button: WinUI's own font dialog, not
 * Windows' old one */
static void font_browse(ConfigBox *cb, dlgcontrol *ctrl)
{
    dlgparam *dp = &cb->dp;
    ConfigCtrl *c = find_ctrl(cb, ctrl);
    FontSpec *fs = c->fs;

    struct font_list fl = { NULL, 0, 0, dp->fixed_pitch_fonts };
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    HDC hdc = GetDC(0);
    EnumFontFamiliesExW(hdc, &lf, font_listed, (LPARAM)&fl, 0);
    ReleaseDC(0, hdc);
    if (fl.n)
        qsort(fl.names, fl.n, sizeof(*fl.names), cmp_wide_ci);

    wchar_t family[LF_FACESIZE];
    wchar_t *wname = to_wide(fs->name, false);
    wcsncpy(family, wname, LF_FACESIZE - 1);
    family[LF_FACESIZE - 1] = L'\0';
    sfree(wname);
    bool bold = fs->isbold;
    int points = fs->height;

    if (wui_dlg_pick_font(cb->dlg, L"Font", (const wchar_t *const *)fl.names,
                          fl.n, family, LF_FACESIZE, &bold, &points)) {
        char *name = dup_wc_to_mb(CP_ACP, family, "");
        /* (the character set it had, for the same font; or else any) */
        int charset = !strcmp(name, fs->name) ? fs->charset :
            DEFAULT_CHARSET;
        FontSpec *nfs = fontspec_new(name, bold, points, charset);
        sfree(name);
        dlg_fontsel_set(ctrl, dp, nfs);
        fontspec_free(nfs);
        ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
    }
    for (size_t i = 0; i < fl.n; i++)
        sfree(fl.names[i]);
    sfree(fl.names);
}

/* A colour selector that an event handler asked for */
static void colour_select(ConfigBox *cb, dlgcontrol *ctrl)
{
    dlgparam *dp = &cb->dp;
    /* (with WinUI's colour picker, rather than Windows' old colour
     * dialog box) */
    unsigned rgb = ((unsigned)dp->coloursel_result.r << 16) |
        ((unsigned)dp->coloursel_result.g << 8) | dp->coloursel_result.b;
    if (wui_dlg_pick_colour(cb->dlg, L"Colour", &rgb)) {
        dp->coloursel_result.r = (unsigned char)(rgb >> 16);
        dp->coloursel_result.g = (unsigned char)(rgb >> 8);
        dp->coloursel_result.b = (unsigned char)rgb;
        dp->coloursel_result.ok = true;
    } else
        dp->coloursel_result.ok = false;
    ctrl->handler(ctrl, dp, dp->data, EVENT_CALLBACK);
}

void wui_dlg_cb_widget(void *ctx, void *wctx, WuiWidgetEvent ev)
{
    ConfigBox *cb = (ConfigBox *)ctx;
    dlgparam *dp = &cb->dp;
    dlgcontrol *ctrl = (dlgcontrol *)wctx;

    if (!cb->initialised || !ctrl)
        return;
    if (wctx == &cb->autologon_changed) {
        if (ev == WUI_EV_VALCHANGE) {
            autologon_changed(cb);
            update_state(cb);
        }
        return;
    }
    if ((char *)wctx >= cb->acts && (char *)wctx < cb->acts + ACT_LIMIT) {
        act(cb, (int)((char *)wctx - cb->acts), ev);
        return;
    }
    if (!find_ctrl(cb, ctrl))
        return;

    /*
     * Focus tracking, as in controls.c, for dlg_last_focused. Moving
     * between the parts of one widget (say a file selector's edit box
     * and button) shows up as losing the focus and gaining it again,
     * which mustn't count as having been somewhere else meanwhile.
     */
    if (ev == WUI_EV_FOCUS) {
        if (!dp->focused && dp->lastfocused == ctrl) {
            dp->lastfocused = cb->unfocus_prev;
        } else if (dp->focused != ctrl) {
            if (dp->focused)
                dp->lastfocused = dp->focused;
        }
        dp->focused = ctrl;
        return;
    }
    if (ev == WUI_EV_UNFOCUS) {
        if (dp->focused == ctrl) {
            cb->unfocus_prev = dp->lastfocused;
            dp->lastfocused = dp->focused;
            dp->focused = NULL;
        }
        return;
    }

    if (!ctrl->handler)
        return;

    if (ev == WUI_EV_RESET || ev == WUI_EV_APPLYALL) {
        int key = ctrl_conf_key(ctrl);
        if (key < 0)
            return;
        if (ev == WUI_EV_APPLYALL) {
            apply_to_all(cb, ctrl);
            return;
        }
        conf_key_copy(box_conf(cb), baseline(cb), key);
        dlg_refresh(NULL, dp);
        update_state(cb);
        return;
    }
    if (ev == WUI_EV_ACTION && ctrl->type == CTRL_BUTTON &&
        ctrl->button.isdefault && !before_connect(cb))
        return;

    if (is_merged(cb, ctrl)) {
        if (ev == WUI_EV_SELCHANGE)
            mc_picked(cb);
        if (dp->ended)
            wui_dlg_stop(cb->dlg);
        else
            update_state(cb);
        return;
    }

    /* Any handler may ask for a colour selector, so every event goes
     * on to check for that at the end */
    dp->coloursel_wanted = false;

    switch (ctrl->type) {
      case CTRL_EDITBOX:
        if (ev == WUI_EV_VALCHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        else if (ev == WUI_EV_COMMIT)
            ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
        break;
      case CTRL_RADIO:
      case CTRL_CHECKBOX:
        if (ev == WUI_EV_VALCHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        break;
      case CTRL_BUTTON:
        if (ev == WUI_EV_ACTION)
            ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
        break;
      case CTRL_LISTBOX:
        if (ev == WUI_EV_ACTION)
            ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
        else if (ev == WUI_EV_SELCHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_SELCHANGE);
        else if (ev == WUI_EV_VALCHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        break;
      case CTRL_FILESELECT:
        if (ev == WUI_EV_VALCHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        else if (ev == WUI_EV_BROWSE)
            file_browse(cb, ctrl);
        break;
      case CTRL_FONTSELECT:
        if (ev == WUI_EV_BROWSE)
            font_browse(cb, ctrl);
        break;
    }

    if (dp->coloursel_wanted)
        colour_select(cb, ctrl);

    if (dp->ended)
        wui_dlg_stop(cb->dlg);
    else
        update_state(cb);
}

/* ----------------------------------------------------------------------
 * The application theme (see winui-config.h).
 */

int app_theme_get(void)
{
    DWORD value;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, PUTTY_REG_POS);
    bool got = key && get_reg_dword(key, "AppTheme", &value);
    if (key)
        close_regkey(key);
    return got && value < APP_THEME_LIMIT ? (int)value : APP_THEME_SYSTEM;
}

static void app_theme_set(int theme)
{
    HKEY key = create_regkey(HKEY_CURRENT_USER, PUTTY_REG_POS);
    if (key) {
        put_reg_dword(key, "AppTheme", theme);
        close_regkey(key);
    }
}

/* Its setting reads and writes the registry, not the Conf, and takes
 * effect at once, without waiting for Open or Apply */
static void theme_handler(dlgcontrol *ctrl, dlgparam *dp, void *data,
                          int event)
{
    if (event == EVENT_REFRESH) {
        int theme = app_theme_get();
        for (int i = 0; i < ctrl->radio.nbuttons; i++) {
            if (ctrl->radio.buttondata[i].i == theme) {
                dlg_radiobutton_set(ctrl, dp, i);
                break;
            }
        }
    } else if (event == EVENT_VALCHANGE) {
        int i = dlg_radiobutton_get(ctrl, dp);
        if (i < 0 || i >= ctrl->radio.nbuttons)
            return;
        int theme = ctrl->radio.buttondata[i].i;
        app_theme_set(theme);
        wui_dlg_set_theme(box_of(dp)->dlg, app_theme_base(theme));
        wui_app_theme_changed();
    }
}

/*
 * Moving some of config.c's settings to where they make more sense in
 * this front end (in its controlbox only: they're the same settings,
 * saved as ever). A controlbox keeps its panels in a tree order, each
 * panel's controlsets together and after its parent's, which
 * ctrl_find_path relies on; so what's moved goes at the end of where
 * it's going, as ctrl_getset would have put it.
 */
static int panel_end(struct controlbox *b, const char *path)
{
    /* (as dialog.c's ctrl_find_set, not 'start': the first set after
     * that path and everything under it) */
    int last = 0;
    for (int i = 0; i < b->nctrlsets; i++) {
        int m = ctrl_path_compare(path, b->ctrlsets[i]->pathname);
        if (m < last)
            return i;
        last = m;
    }
    return b->nctrlsets;
}

static void move_sets(struct controlbox *b, const char *from,
                      const char *boxname, const char *to)
{
    size_t flen = strlen(from);
    struct controlset **moved = snewn(b->nctrlsets, struct controlset *);
    int nmoved = 0, n = 0;
    for (int i = 0; i < b->nctrlsets; i++) {
        struct controlset *cs = b->ctrlsets[i];
        /* (the panel, and those under it; or just the one set) */
        bool match = boxname ?
            !strcmp(cs->pathname, from) && cs->boxname &&
            !strcmp(cs->boxname, boxname) :
            !strncmp(cs->pathname, from, flen) &&
            (!cs->pathname[flen] || cs->pathname[flen] == '/');
        if (match)
            moved[nmoved++] = cs;
        else
            b->ctrlsets[n++] = cs;
    }
    b->nctrlsets = n;
    for (int i = 0; i < nmoved; i++) {
        struct controlset *cs = moved[i];
        char *path = dupcat(to, cs->pathname + flen);
        sfree(cs->pathname);
        cs->pathname = path;
        int at = panel_end(b, path);
        memmove(&b->ctrlsets[at + 1], &b->ctrlsets[at],
                (b->nctrlsets - at) * sizeof(*b->ctrlsets));
        b->ctrlsets[at] = cs;
        b->nctrlsets++;
    }
    sfree(moved);
}

static void move_ctrls(struct controlbox *b)
{
    /* Session logging is of what the terminal shows (or of the SSH
     * traffic behind it), not of where the session's connected to */
    move_sets(b, "Session/Logging", NULL, "Terminal/Logging");

    /* Close window on exit: here, it's the tab that closes, so it goes
     * with the window's other behaviour, as what to do when the
     * session ends */
    move_sets(b, "Session", "otheropts", "Window/Behaviour");
    int i = ctrl_find_path(b, "Window/Behaviour", -1);
    for (; i >= 0; i = ctrl_find_path(b, "Window/Behaviour", i)) {
        struct controlset *cs = b->ctrlsets[i];
        if (!cs->boxname || strcmp(cs->boxname, "otheropts"))
            continue;
        for (size_t j = 0; j < cs->ncontrols; j++) {
            dlgcontrol *c = cs->ctrls[j];
            if (c->type == CTRL_RADIO && c->label &&
                !strcmp(c->label, "Close window on exit:")) {
                sfree(c->label);
                c->label = dupstr("Close the tab when the session ends:");
            }
        }
    }
}

/* The setting for the pools of text to paste again (see show_pool in
 * winui-window.c), at the end of the Terminal panel */
static void add_pool_ctrls(struct controlbox *b)
{
    struct controlset *s = ctrl_getset(b, "Terminal", "copypool",
                                       "Pools of copied text");
    ctrl_editbox(s, "Copies to keep, in the tab's history and the "
                 "session's pool (0 to keep none)", NO_SHORTCUT, 20,
                 HELPCTX(no_help), conf_editbox_handler,
                 I(CONF_copy_pool_size), ED_INT);
}

/* And for JoliPuTTY's own keyboard shortcuts (see handle_key_message in
 * winui-window.c), for a session where they'd be in the way */
static void add_shortcut_ctrls(struct controlbox *b)
{
    struct controlset *s = ctrl_getset(b, "Terminal", "wuishortcuts",
                                       "JoliPuTTY's keyboard shortcuts");
    ctrl_checkbox(s, "Send them to the server instead (Ctrl+comma, "
                  "Ctrl+Alt+V, Ctrl+Alt+1 to 9, Alt+arrows, Alt+Shift+plus "
                  "and minus, Ctrl+Shift+W, Ctrl+Shift+Space)", NO_SHORTCUT,
                  HELPCTX(no_help), conf_checkbox_handler,
                  I(CONF_no_wui_shortcuts));
}

static void add_theme_ctrls(struct controlbox *b)
{
    static const char path[] = "Window/Colours";
    struct controlset *s = ctrl_getset(b, path, "apptheme",
                                       "Application theme");
    /* (in Windows Terminal's order) */
    ctrl_radiobuttons(s, "Theme, for all sessions:", NO_SHORTCUT, 3,
                      HELPCTX(no_help), theme_handler, P(NULL),
                      "Dark", NO_SHORTCUT, I(APP_THEME_DARK),
                      "Light", NO_SHORTCUT, I(APP_THEME_LIGHT),
                      "Use Windows theme", NO_SHORTCUT,
                      I(APP_THEME_SYSTEM),
                      "Dark (legacy)", NO_SHORTCUT,
                      I(APP_THEME_DARK_LEGACY),
                      "Light (legacy)", NO_SHORTCUT,
                      I(APP_THEME_LIGHT_LEGACY),
                      "Use Windows theme (legacy)", NO_SHORTCUT,
                      I(APP_THEME_SYSTEM_LEGACY));

    /* First on its panel (after the panel's title), not last */
    int first = ctrl_find_path(b, path, -1);
    if (first < 0)
        return;
    if (!b->ctrlsets[first]->boxname)
        first++;
    for (int i = first; i < b->nctrlsets; i++) {
        if (b->ctrlsets[i] == s) {
            memmove(&b->ctrlsets[first + 1], &b->ctrlsets[first],
                    (i - first) * sizeof(*b->ctrlsets));
            b->ctrlsets[first] = s;
            break;
        }
    }
}

/* ----------------------------------------------------------------------
 * Running the box.
 */

static bool run_config_box(Conf *conf, HWND owner, bool midsession,
                           int protocol, int protcfginfo,
                           const char *session, char **launched,
                           bool defaults, bool destination)
{
    ConfigBox *cb = snew(ConfigBox);
    memset(cb, 0, sizeof(*cb));
    cb->current = -1;

    cb->midsession = midsession;
    cb->session = midsession && session ? dupstr(session) : NULL;
    /* (the Home page as it was left) */
    cb->hidden = home_hidden_get();
    reload_sessions(cb);
    load_collapsed(cb);
    cb->target = !midsession && session ? TARGET_SESSION : TARGET_NEW;
    cb->target_name = !midsession && session ? dupstr(session) : NULL;
    cb->saved = conf_copy(conf);
    cb->defaults = conf_new();
    do_defaults(NULL, cb->defaults);
    cb->builtin = conf_new();
    load_open_settings(NULL, cb->builtin);
    cb->ctrlbox = ctrl_new_box();
    dp_init(&cb->dp);
    cb->dp.fe = &wui_dlg_vt;
    /* (with the host name, port and connection type mid-session too:
     * once the session's over, for restarting it somewhere else, or
     * saving that; while it's connected, just to see, as changing them
     * wouldn't move it) */
    cb->dest_readonly = midsession && !destination;
    setup_config_box_dest(cb->ctrlbox, midsession, protocol, protcfginfo,
                          true);
    /* (no Help button: this PuTTY has no help file of its own) */
    win_setup_config_box(cb->ctrlbox, &cb->dp.hwnd, false, midsession,
                         protocol);
    add_theme_ctrls(cb->ctrlbox);
    add_pool_ctrls(cb->ctrlbox);
    add_shortcut_ctrls(cb->ctrlbox);
    move_ctrls(cb->ctrlbox);
    cb->dp.wintitle = dupprintf(midsession ? "%s Reconfiguration" :
                                "%s Configuration", appname);
    cb->dp.errtitle = dupprintf("%s Error", appname);
    cb->dp.data = conf;
    dlg_auto_set_fixed_pitch_flag(&cb->dp);

    wchar_t *wtitle = to_wide(cb->dp.wintitle, false);
    cb->dlg = wui_dlg_new(owner, wtitle,
                          LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)), cb);
    sfree(wtitle);
    cb->dp.hwnd = wui_dlg_hwnd(cb->dlg);
    wui_dlg_set_theme(cb->dlg, app_theme_base(app_theme_get()));
    {
        /* (the manual's section on this box) */
        wchar_t *url = wui_help_url("joliputty.html#jp-config");
        wui_dlg_set_help(cb->dlg, url);
        sfree(url);
    }

    build_path(cb, "", true);          /* the buttons along the bottom */
    if (midsession) {
        /* (Apply: with a saved session, not the main button, which is
         * Apply and save; see build_page_actions) */
        for (size_t i = 0; i < cb->nctrls; i++)
            if (cb->ctrls[i].ctrl->type == CTRL_BUTTON &&
                cb->ctrls[i].ctrl->button.isdefault && cb->ctrls[i].w) {
                cb->apply_w = cb->ctrls[i].w;
                update_apply(cb);
            }
    }
    if (!midsession) {
        /* (Open: it's what it does) */
        for (size_t i = 0; i < cb->nctrls; i++)
            if (cb->ctrls[i].ctrl->type == CTRL_BUTTON &&
                cb->ctrls[i].ctrl->button.isdefault && cb->ctrls[i].w) {
                cb->connect_w = cb->ctrls[i].w;
                wui_w_set_label(cb->connect_w, L"Connect");
            }
        wui_dlg_add_category(cb->dlg, L"Sessions", 0xE80F, HOME_ID);
    }
    build_categories(cb);
    assert(cb->npaths > 0);   /* config.c must have given us _something_ */

    /* Start in the first control of the first panel (mid-session), or
     * the Session (Address) page's, the host name: for a new session,
     * as that's what it's for (the saved ones being a click away, on
     * the Sessions page, or on the title bar's arrow), or for a session
     * that's been picked already */
    if (defaults) {
        /* (just the defaults for new sessions, from mid-session) */
        set_target(cb, TARGET_DEFAULTS, NULL);
        show_category(cb, session_page(cb));
    } else {
        show_category(cb, midsession ? 0 : session_page(cb));
        for (size_t i = 0; i < cb->nctrls; i++) {
            if (!cb->ctrls[i].footer) {
                dlg_set_focus(cb->ctrls[i].ctrl, &cb->dp);
                break;
            }
        }
    }

    cb->initialised = true;
    wui_dlg_run(cb->dlg);
    /* (a name typed, and the box ended without leaving it first) */
    commit_session_name(cb, true);

    bool ret = cb->dp.ended && cb->dp.endresult;
    if (launched && midsession)
        /* (mid-session, the tab's saved session's new name, if it's
         * been renamed, or "" if it's been deleted, whether the changes
         * were applied or not) */
        *launched = !cb->session_moved ? NULL :
            dupstr(cb->session ? cb->session : "");
    else if (launched)
        *launched = ret && cb->launched ? dupstr(cb->launched) : NULL;

    free_box(cb);
    return ret;
}

/* Close a box, and free everything in it */
static void free_box(ConfigBox *cb)
{
    wui_dlg_free(cb->dlg);
    for (size_t i = 0; i < cb->nctrls; i++)
        free_ctrl(&cb->ctrls[i]);
    sfree(cb->ctrls);
    sfree(cb->paths);
    mc_forget(&cb->mc);
    sfree(cb->target_name);
    sfree(cb->session);
    sfree(cb->launched);
    conf_free(cb->saved);
    conf_free(cb->defaults);
    conf_free(cb->builtin);
    sessions_free(cb->sess, cb->nsess);
    folders_free(cb->folders, cb->nfolders);
    sfree(cb->new_folder);
    clear_rowkeys(cb);
    sfree(cb->rowkeys);
    for (size_t i = 0; i < cb->ncollapsed; i++)
        sfree(cb->collapsed[i]);
    sfree(cb->collapsed);
    forget_undo(cb);
    sfree(cb->prompt_for);
    ctrl_free_box(cb->ctrlbox);
    dp_cleanup(&cb->dp);
    sfree(cb);
}

/*
 * The host CA box (Connection > SSH > Host keys, Configure host CAs),
 * which config.c asks for through show_ca_config_box: its controls,
 * from setup_ca_config_box, in a box like this one, over it, with its
 * one panel as its one page.
 */
static void wd_ca_config(dlgparam *dp)
{
    ConfigBox *owner = box_of(dp);
    ConfigBox *cb = snew(ConfigBox);
    memset(cb, 0, sizeof(*cb));
    cb->current = -1;
    cb->ca = true;
    /* (the settings it doesn't have, as nothing here is one) */
    cb->saved = conf_new();
    cb->defaults = conf_new();
    cb->builtin = conf_new();
    Conf *conf = conf_new();
    cb->ctrlbox = ctrl_new_box();
    dp_init(&cb->dp);
    cb->dp.fe = &wui_dlg_vt;
    setup_ca_config_box(cb->ctrlbox);
    cb->dp.wintitle = dupprintf("%s trusted host certification authorities",
                                appname);
    cb->dp.errtitle = dupprintf("%s Error", appname);
    cb->dp.data = conf;

    wchar_t *wtitle = to_wide(cb->dp.wintitle, false);
    cb->dlg = wui_dlg_new(owner->dp.hwnd, wtitle,
                          LoadIcon(hinst, MAKEINTRESOURCE(IDI_CFGICON)), cb);
    sfree(wtitle);
    cb->dp.hwnd = wui_dlg_hwnd(cb->dlg);
    wui_dlg_set_theme(cb->dlg, app_theme_base(app_theme_get()));

    build_path(cb, "", true);          /* (Done, which Escape is) */
    build_categories(cb);
    show_category(cb, 0);
    dlg_refresh(NULL, &cb->dp);
    cb->initialised = true;
    wui_dlg_run(cb->dlg);

    free_box(cb);
    conf_free(conf);
}

int app_theme_base(int theme)
{
    switch (theme) {
      case APP_THEME_LIGHT: case APP_THEME_LIGHT_LEGACY: return 1;
      case APP_THEME_DARK: case APP_THEME_DARK_LEGACY: return 2;
      default: return 0;
    }
}

bool app_theme_tinted_tab(int theme)
{
    return theme < APP_THEME_SYSTEM_LEGACY;
}

bool wui_do_config(HWND owner, Conf *conf, const char *session,
                   char **launched)
{
    return run_config_box(conf, owner, false, 0, 0, session, launched,
                          false, false);
}

bool wui_do_reconfig(HWND owner, Conf *conf, int protcfginfo,
                     const char *session, bool disconnected,
                     char **renamed)
{
    Conf *backup_conf = conf_copy(conf);
    bool ret = run_config_box(conf, owner, true,
                              conf_get_int(conf, CONF_protocol), protcfginfo,
                              session, renamed, false, disconnected);
    if (!ret)
        conf_copy_into(conf, backup_conf);
    conf_free(backup_conf);
    return ret;
}
