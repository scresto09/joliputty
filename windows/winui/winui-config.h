/*
 * winui-config.h: the WinUI 3 configuration box (winui-config.c), as
 * used by the rest of the WinUI front end in place of dialog.c's
 * do_config and do_reconfig. It can only run once the XAML
 * application is going.
 */

#ifndef PUTTY_WINUI_WINUI_CONFIG_H
#define PUTTY_WINUI_WINUI_CONFIG_H

/*
 * Both return true if the user pressed Connect (or Apply), false if
 * they cancelled. 'owner' (which may be NULL) is disabled meanwhile.
 *
 * For a new session, the box starts on its Sessions page, or if
 * 'session' is the name of a saved one (already in 'conf'), editing
 * that. If the user connects to a saved session, its name goes in
 * *launched (to be freed), or else NULL does. Mid-session, 'session'
 * is the one the tab was opened from, if any, for saving changes to.
 */
bool wui_do_config(HWND owner, Conf *conf, const char *session,
                   char **launched);
/* (and if the tab's saved session was renamed from the box, its new
 * name, in 'renamed', to be freed, or "" if it was deleted; or NULL) */
/* ('disconnected': the session's over, so its host name, port and
 * connection type can be changed, for restarting it) */
bool wui_do_reconfig(HWND owner, Conf *conf, int protcfginfo,
                     const char *session, bool disconnected,
                     char **renamed);

/* Provided by winui-window.c: open a saved session in a new window */
void wui_launch_session_window(const char *session);

/* Provided by winui-window.c: the address of a page of the manual (to
 * free), with "#section" if wanted (NULL for its contents), and showing
 * it in the browser */
wchar_t *wui_help_url(const char *page);
void wui_show_help(HWND hwnd, const char *page);

/*
 * The application theme: whether the window (tab strip, menus, this
 * box) is light or dark, or follows Windows; and whether the selected
 * tab takes on its terminal's background colour, as in Windows
 * Terminal (all but the 'legacy' ones). It's the same for every
 * session, so it's kept in the registry beside them, not in a Conf.
 */
enum {
    APP_THEME_SYSTEM, APP_THEME_LIGHT, APP_THEME_DARK,
    APP_THEME_SYSTEM_LEGACY, APP_THEME_LIGHT_LEGACY, APP_THEME_DARK_LEGACY,
    APP_THEME_LIMIT
};
int app_theme_get(void);
/* As wui_set_theme and wui_dlg_set_theme take it: 0 to follow
 * Windows, 1 light, 2 dark */
int app_theme_base(int theme);
bool app_theme_tinted_tab(int theme);

/* Provided by winui-window.c: the theme has changed, so show it */
void wui_app_theme_changed(void);

#endif /* PUTTY_WINUI_WINUI_CONFIG_H */
