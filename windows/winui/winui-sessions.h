/*
 * winui-sessions.h: the saved sessions, as the WinUI configuration
 * box manages them: finding out what there is, and saving, copying,
 * renaming and deleting them (along with their hidden Automate logon
 * commands, which live in the Windows Credential Manager, and what
 * this front end remembers about them as well: which are favorites, and
 * when each was last used), exporting and importing them, and
 * comparing settings.
 *
 * The sessions themselves are stored just as PuTTY always has, so
 * that other versions of PuTTY can still use them. A session's name
 * may contain '#', which the box shows as folders, as PuTTY-ng does:
 * "Clients#ACME#web" is the session "web" in the folder "ACME" in the
 * folder "Clients".
 */

#ifndef PUTTY_WINUI_SESSIONS_H
#define PUTTY_WINUI_SESSIONS_H

#include <time.h>

#define DEFAULTS_NAME "Default Settings"

/* What separates the folders in a session's name */
#define FOLDER_SEP '#'
#define FOLDER_SEP_S "#"
#define FOLDER_SEP_W L'#'
/* What a session's or a folder's own name can't have in it: the
 * separator, and what PuTTY-ng doesn't allow there either */
#define NAME_REFUSED_W L"#/\\"

/* A session's name, or a folder's path, as shown to the user, with
 * '/' between its folders, which reads better than '#' (to free) */
char *name_shown(const char *name);

typedef struct SessionInfo {
    char *name;
    char *host, *user;                 /* (as saved, perhaps empty) */
    int port;
    char *protocol;                    /* (as saved: "ssh", "telnet"...) */
    time_t last_used;                  /* 0 if never (that we know of) */
    bool favorite;
} SessionInfo;

/* The saved sessions (not Default Settings), in order of name */
SessionInfo *sessions_get(size_t *n);
void sessions_free(SessionInfo *s, size_t n);
bool session_exists(const char *name);
/* What a session connects to, to show under its name: "user@host:port
 * · SSH" (to free) */
wchar_t *session_subtitle(const SessionInfo *si);

/*
 * Save a session (DEFAULTS_NAME for the default settings), with its
 * hidden Automate logon commands in the Credential Manager, which
 * 'conf' then refers to. Returns an error message to free, or NULL.
 */
char *session_store(const char *name, Conf *conf);
/* Load a session, with its hidden Automate logon commands as
 * themselves, so that it can be stored again under any name */
bool session_load_plain(const char *name, Conf *conf);
/* These return an error message to free, or NULL */
char *session_copy(const char *from, const char *to);
char *session_rename(const char *from, const char *to);
/* (and its commands in the Credential Manager, and what we remember
 * about it) */
void session_delete(const char *name);

/*
 * A host name typed in, to connect to: "user@host:port", say, by SSH
 * unless it starts with another kind of connection's name and "://"
 * ("telnet://host", or "serial://COM1:115200", with a speed), put
 * into 'conf' (perhaps the defaults). Returns an error message to
 * free, or NULL.
 */
char *session_quick(Conf *conf, const char *text);

/* It's just been used (to connect) */
void session_touch(const char *name);
void session_set_last_used(const char *name, time_t when);
void session_set_favorite(const char *name, bool favorite);

/*
 * Folders. A session's folder is just the part of its name before
 * the last '#', so a folder with sessions in it needs no storing; but
 * one made to put sessions in later is remembered here (as its path,
 * "Clients#ACME", say), until it's removed. The empty folders that
 * PuTTY-ng makes (as sessions with names ending in '#', which aren't
 * sessions, so sessions_get leaves them out) count too, though none
 * are made here. folders_get returns them in order of name;
 * folder_remove removes the folders inside it too, and folder_rename
 * moves them along with it (to the top level, if 'to' is "").
 */
char **folders_get(size_t *n);
void folders_free(char **f, size_t n);
void folder_add(const char *path);
void folder_remove(const char *path);
void folder_rename(const char *from, const char *to);

/*
 * Pools of text kept to paste again (see show_pool in winui-window.c):
 * a saved session's, if 'session' is its name, or the one for all of
 * them, if it's NULL. Newest first; pool_set stores them so.
 */
wchar_t **pool_get(const char *session, size_t *n);
void pool_set(const char *session, wchar_t *const *items, size_t n);
void pool_free(wchar_t **items, size_t n);

/* What's closed on the Home page, to be so next time too: its
 * sections (as a set of flags of the box's own), and its folders */
unsigned home_hidden_get(void);
void home_hidden_set(unsigned hidden);
char **home_collapsed_get(size_t *n);       /* (free with folders_free) */
void home_collapsed_set(char *const *folders, size_t n);
/* ... and which of those sections the title bar's pop-up of sessions
 * leaves out altogether */
unsigned popup_omitted_get(void);
void popup_omitted_set(unsigned omitted);

/* Export one session (or all of them, if name is NULL) to a .reg file,
 * or import a file of them; each returns an error message to free, or
 * NULL. Importing only takes files that are just PuTTY sessions. */
char *sessions_export(const char *name, const wchar_t *file);
char *sessions_import(const wchar_t *file, int *count);

/* Comparing and copying settings, one or all of them */
bool conf_key_equal(Conf *a, Conf *b, int key);
void conf_key_copy(Conf *to, Conf *from, int key);
bool conf_equal(Conf *a, Conf *b);

#endif /* PUTTY_WINUI_SESSIONS_H */
