/*
 * winui-sessions.c: managing the saved sessions, for the WinUI
 * configuration box (see winui-sessions.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#include "putty.h"
#include "storage.h"

#include <wincrypt.h>

#include "winui-sessions.h"
#include "autologon.h"

/* Where we remember things about sessions that PuTTY itself doesn't:
 * a value for each session, named after it */
#define FAVORITES_KEY PUTTY_REG_POS "\\WinUI\\Favorites"
#define LAST_USED_KEY PUTTY_REG_POS "\\WinUI\\LastUsed"
#define SESSIONS_KEY PUTTY_REG_POS "\\Sessions"
/* ... and the folders made to put sessions in: a value for each (with
 * their names separated by '#'; not where they were kept when that was
 * '/', so as to start again) */
#define FOLDERS_KEY PUTTY_REG_POS "\\WinUI\\EmptyFolders"
/* ... and how the Home page was left */
#define WINUI_KEY PUTTY_REG_POS "\\WinUI"
/* ... and the pools of text to paste: a value for each session (and
 * one in WINUI_KEY for all of them), a list of strings, encrypted for
 * the user with DPAPI (see pool_get). (Not where they were kept in the
 * clear, before that, which are left alone.) */
#define POOLS_KEY PUTTY_REG_POS "\\WinUI\\SecurePools"
#define COMMON_POOL "SecureCommonPool"

/* ----------------------------------------------------------------------
 * What we remember about sessions.
 */

static void put_info(const char *keyname, const char *name, const char *val)
{
    HKEY key = create_regkey(HKEY_CURRENT_USER, keyname);
    if (!key)
        return;
    if (val)
        put_reg_sz(key, name, val);
    else
        RegDeleteValueA(key, name);
    close_regkey(key);
}

static char *get_info(const char *keyname, const char *name)
{
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, keyname);
    if (!key)
        return NULL;
    char *val = get_reg_sz(key, name);
    close_regkey(key);
    return val;
}

void session_set_last_used(const char *name, time_t when)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)when);
    put_info(LAST_USED_KEY, name, when ? buf : NULL);
}

void session_touch(const char *name)
{
    if (name && *name && strcmp(name, DEFAULTS_NAME))
        session_set_last_used(name, time(NULL));
}

void session_set_favorite(const char *name, bool favorite)
{
    put_info(FAVORITES_KEY, name, favorite ? "1" : NULL);
}

/* Forget what we remember about a session, or move it to a new name */
static void move_info(const char *from, const char *to)
{
    static const char *const keys[] = { FAVORITES_KEY, LAST_USED_KEY };
    for (size_t i = 0; i < lenof(keys); i++) {
        char *val = get_info(keys[i], from);
        if (val) {
            put_info(keys[i], from, NULL);
            if (to)
                put_info(keys[i], to, val);
            sfree(val);
        }
    }
}

/* ----------------------------------------------------------------------
 * Pools of text to paste.
 */

/*
 * The pools are encrypted with DPAPI, as the Credential Manager's own
 * contents are, for the user's account on this computer: another
 * account, or the registry copied elsewhere, can't read them. (From
 * crypt32.dll, loaded when it's wanted.)
 */
/* (one type for both: their second argument differs, a description in
 * and a description out, but it's a pointer either way, and the second
 * is passed NULL) */
typedef BOOL (WINAPI *crypt_fn_t)(DATA_BLOB *, const void *, DATA_BLOB *,
                                  PVOID, PVOID, DWORD, DATA_BLOB *);

static bool pool_crypt(bool encrypt, const void *in, size_t inlen,
                       DATA_BLOB *out)
{
    static crypt_fn_t protect, unprotect;
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE dll = load_system32_dll("crypt32.dll");
        if (dll) {
            protect = (crypt_fn_t)(void (*)(void))GetProcAddress(
                dll, "CryptProtectData");
            unprotect = (crypt_fn_t)(void (*)(void))GetProcAddress(
                dll, "CryptUnprotectData");
        }
    }
    DATA_BLOB blob = { (DWORD)inlen, (BYTE *)in };
    if (encrypt)
        return protect && protect(&blob, L"JoliPuTTY pool", NULL,
                                  NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, out);
    return unprotect && unprotect(&blob, NULL, NULL, NULL, NULL,
                                  CRYPTPROTECT_UI_FORBIDDEN, out);
}

wchar_t **pool_get(const char *session, size_t *nout)
{
    wchar_t **items = NULL;
    size_t n = 0, size = 0;
    *nout = 0;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER,
                              session ? POOLS_KEY : WINUI_KEY);
    if (!key)
        return NULL;
    wchar_t *wname = dup_mb_to_wc(CP_ACP, session ? session : COMMON_POOL);
    DWORD type, bytes = 0;
    if (RegQueryValueExW(key, wname, NULL, &type, NULL, &bytes) ==
        ERROR_SUCCESS && type == REG_BINARY && bytes) {
        BYTE *enc = snewn(bytes, BYTE);
        DATA_BLOB plain;
        if (RegQueryValueExW(key, wname, NULL, &type, enc, &bytes) ==
            ERROR_SUCCESS && pool_crypt(false, enc, bytes, &plain)) {
            /* (a list of strings, each ended by a NUL, and then one more,
             * as in a REG_MULTI_SZ; trusting none of that) */
            size_t nw = plain.cbData / sizeof(wchar_t);
            wchar_t *buf = snewn(nw + 2, wchar_t);
            memcpy(buf, plain.pbData, nw * sizeof(wchar_t));
            buf[nw] = buf[nw + 1] = L'\0';
            for (wchar_t *p = buf; *p; p += wcslen(p) + 1) {
                sgrowarray(items, size, n);
                items[n++] = dupwcs(p);
            }
            smemclr(buf, (nw + 2) * sizeof(wchar_t));
            sfree(buf);
            SecureZeroMemory(plain.pbData, plain.cbData);
            LocalFree(plain.pbData);
        }
        sfree(enc);
    }
    sfree(wname);
    close_regkey(key);
    *nout = n;
    return items;
}

void pool_set(const char *session, wchar_t *const *items, size_t n)
{
    HKEY key = create_regkey(HKEY_CURRENT_USER,
                             session ? POOLS_KEY : WINUI_KEY);
    if (!key)
        return;
    wchar_t *wname = dup_mb_to_wc(CP_ACP, session ? session : COMMON_POOL);
    size_t total = 1;
    for (size_t i = 0; i < n; i++)
        if (*items[i])
            total += wcslen(items[i]) + 1;
    if (total == 1) {
        RegDeleteValueW(key, wname);
    } else {
        wchar_t *buf = snewn(total, wchar_t), *p = buf;
        for (size_t i = 0; i < n; i++) {
            if (!*items[i])
                continue;              /* (which would end the list) */
            size_t len = wcslen(items[i]) + 1;
            memcpy(p, items[i], len * sizeof(wchar_t));
            p += len;
        }
        *p = L'\0';
        DATA_BLOB enc;
        if (pool_crypt(true, buf, total * sizeof(wchar_t), &enc)) {
            RegSetValueExW(key, wname, 0, REG_BINARY, enc.pbData,
                           enc.cbData);
            LocalFree(enc.pbData);
        }
        smemclr(buf, total * sizeof(wchar_t));
        sfree(buf);
    }
    sfree(wname);
    close_regkey(key);
}

void pool_free(wchar_t **items, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sfree(items[i]);
    sfree(items);
}

/* A session's pool goes with it when it's renamed, or deleted */
static void pool_move(const char *from, const char *to)
{
    size_t n;
    wchar_t **items = pool_get(from, &n);
    if (!items)
        return;
    pool_set(from, NULL, 0);
    if (to)
        pool_set(to, items, n);
    pool_free(items, n);
}

/* ----------------------------------------------------------------------
 * The list of sessions.
 */

static char *read_str(settings_r *sr, const char *key)
{
    char *s = sr ? read_setting_s(sr, key) : NULL;
    return s ? s : dupstr("");
}

SessionInfo *sessions_get(size_t *nout)
{
    struct sesslist sl;
    get_sesslist(&sl, true);
    SessionInfo *s = snewn(sl.nsessions ? sl.nsessions : 1, SessionInfo);
    size_t n = 0;
    for (int i = 0; i < sl.nsessions; i++) {
        const char *name = sl.sessions[i];
        size_t len = strlen(name);
        /* (nor PuTTY-ng's folders: see folders_get) */
        if (!strcmp(name, DEFAULTS_NAME) || !len ||
            name[len - 1] == FOLDER_SEP)
            continue;
        SessionInfo *si = &s[n++];
        si->name = dupstr(name);
        settings_r *sr = open_settings_r(name);
        si->host = read_str(sr, "HostName");
        si->user = read_str(sr, "UserName");
        si->protocol = read_str(sr, "Protocol");
        si->port = sr ? read_setting_i(sr, "PortNumber", 0) : 0;
        close_settings_r(sr);
        char *val = get_info(LAST_USED_KEY, name);
        si->last_used = val ? (time_t)strtoll(val, NULL, 10) : 0;
        sfree(val);
        val = get_info(FAVORITES_KEY, name);
        si->favorite = val != NULL;
        sfree(val);
    }
    get_sesslist(&sl, false);
    *nout = n;
    return s;
}

void sessions_free(SessionInfo *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sfree(s[i].name);
        sfree(s[i].host);
        sfree(s[i].user);
        sfree(s[i].protocol);
    }
    sfree(s);
}

/* "user@host:port · SSH" */
wchar_t *session_subtitle(const SessionInfo *si)
{
    static const struct {
        const char *name;
        const wchar_t *label;
        int port;
    } protos[] = {
        { "ssh", L"SSH", 22 }, { "telnet", L"Telnet", 23 },
        { "rlogin", L"Rlogin", 513 }, { "raw", L"Raw", 0 },
        { "serial", L"Serial", 0 }, { "supdup", L"SUPDUP", 95 },
        { "ssh-connection", L"Bare ssh-connection", 0 },
    };
    const wchar_t *label = NULL;
    int defport = 0;
    for (size_t i = 0; i < lenof(protos); i++)
        if (!strcmp(si->protocol, protos[i].name)) {
            label = protos[i].label;
            defport = protos[i].port;
        }

    strbuf *sb = strbuf_new();
    if (!*si->host) {
        put_fmt(sb, "(no host name)");
    } else {
        if (*si->user && !strchr(si->host, '@'))
            put_fmt(sb, "%s@", si->user);
        put_fmt(sb, "%s", si->host);
        if (si->port && si->port != defport && strcmp(si->protocol, "serial"))
            put_fmt(sb, ":%d", si->port);
    }
    wchar_t *where = dup_mb_to_wc(CP_ACP, sb->s);
    strbuf_free(sb);
    wchar_t *proto = label ? dupwcs(label) : dup_mb_to_wc(CP_ACP,
                                                          si->protocol);
    wchar_t *ret = *proto ? dupwcscat(where, L"  \x00B7  ", proto) :
        dupwcs(where);
    sfree(proto);
    sfree(where);
    return ret;
}

char *name_shown(const char *name)
{
    char *s = dupstr(name);
    for (char *p = s; *p; p++)
        if (*p == FOLDER_SEP)
            *p = '/';
    return s;
}

bool session_exists(const char *name)
{
    settings_r *sr = open_settings_r(name);
    if (!sr)
        return false;
    close_settings_r(sr);
    return true;
}

/* ----------------------------------------------------------------------
 * Saving, loading, copying, renaming, deleting.
 */

char *session_store(const char *name, Conf *conf)
{
    size_t n;
    AutologonStep *steps = autologon_get_steps(conf, &n);
    /* (Get them all before removing the session's old ones, which may
     * be where some of them are) */
    char **plain = snewn(n ? n : 1, char *);
    for (size_t i = 0; i < n; i++)
        plain[i] = steps[i].hide ? autologon_command(&steps[i]) : NULL;
    autologon_cred_delete_session(name);
    bool failed = false;
    for (size_t i = 0; i < n; i++) {
        if (!steps[i].hide)
            continue;
        burnstr(steps[i].command);
        steps[i].command = NULL;
        steps[i].stored = false;
        if (plain[i] && *plain[i]) {
            char *cname = autologon_cred_name(name, i);
            if (autologon_cred_write(cname, plain[i])) {
                steps[i].command = cname;
                steps[i].stored = true;
            } else {
                sfree(cname);
                failed = true;
            }
        }
        if (!steps[i].command)
            steps[i].command = dupstr("");
        if (plain[i])
            burnstr(plain[i]);
    }
    sfree(plain);
    autologon_set_steps(conf, steps, n);
    autologon_free_steps(steps, n);

    char *err = save_settings(name, conf);
    if (!err && failed)
        err = dupstr("A hidden Automate logon command couldn't be put in "
                     "the Windows Credential Manager, so it hasn't been "
                     "saved.");
    return err;
}

bool session_load_plain(const char *name, Conf *conf)
{
    if (!load_settings(name, conf))
        return false;
    size_t n;
    AutologonStep *steps = autologon_get_steps(conf, &n);
    bool any = false;
    for (size_t i = 0; i < n; i++) {
        if (!steps[i].stored)
            continue;
        char *plain = autologon_command(&steps[i]);
        burnstr(steps[i].command);
        steps[i].command = plain ? plain : dupstr("");
        steps[i].stored = false;
        any = true;
    }
    if (any)
        autologon_set_steps(conf, steps, n);
    autologon_free_steps(steps, n);
    return true;
}

char *session_copy(const char *from, const char *to)
{
    Conf *conf = conf_new();
    char *err = NULL;
    if (!session_load_plain(from, conf))
        err = dupprintf("Couldn't load the session \"%s\"", from);
    else
        err = session_store(to, conf);
    conf_free(conf);
    return err;
}

char *session_rename(const char *from, const char *to)
{
    char *err = session_copy(from, to);
    if (err)
        return err;
    del_settings(from);
    autologon_cred_delete_session(from);
    move_info(to, NULL);               /* (anything left over there) */
    move_info(from, to);
    pool_move(from, to);
    return NULL;
}

void session_delete(const char *name)
{
    del_settings(name);
    autologon_cred_delete_session(name);
    move_info(name, NULL);
    pool_move(name, NULL);
}

/* ----------------------------------------------------------------------
 * Connecting to a host name typed in.
 */

char *session_quick(Conf *conf, const char *text)
{
    const char *p = text + strspn(text, " \t");
    const BackendVtable *vt = backend_vt_from_proto(PROT_SSH);
    const char *sep = strstr(p, "://");
    if (sep) {
        char *scheme = dupprintf("%.*s", (int)(sep - p), p);
        for (char *q = scheme; *q; q++)
            *q = tolower((unsigned char)*q);
        vt = backend_vt_from_name(scheme);
        if (!vt) {
            char *err = dupprintf("%s doesn't know a kind of connection "
                                  "called \"%s\"", appname, scheme);
            sfree(scheme);
            return err;
        }
        sfree(scheme);
        p = sep + 3;
    }
    if (!vt)
        return dupprintf("%s can't make SSH connections", appname);
    conf_set_int(conf, CONF_protocol, vt->protocol);

    char *host = dupstr(p);
    size_t len = strlen(host);
    while (len && strchr(" \t/", host[len - 1]))
        host[--len] = '\0';

    /* A port number on the end (or for a serial line, its speed), but
     * not the end of an IPv6 address, unless that's in brackets */
    int port = -1;
    char *colon = strrchr(host, ':');
    if (colon && colon[1] && !colon[1 + strspn(colon + 1, "0123456789")]) {
        char *at = strrchr(host, '@');
        char *h = at ? at + 1 : host;
        bool bracketed = *h == '[' && colon[-1] == ']';
        if (bracketed || strchr(h, ':') == colon) {
            port = atoi(colon + 1);
            *colon = '\0';
            if (bracketed) {
                memmove(h, h + 1, strlen(h));
                h[strlen(h) - 1] = '\0';
            }
        }
    }

    char *err = NULL;
    if (vt->protocol == PROT_SERIAL) {
        if (!*host)
            err = dupstr("There's no serial line to connect to: COM1, say");
        else
            conf_set_str(conf, CONF_serline, host);
        if (port > 0)
            conf_set_int(conf, CONF_serspeed, port);
    } else if (!*host) {
        err = dupstr("There's no host name to connect to");
    } else {
        conf_set_str(conf, CONF_host, host);
        if (port > 0)
            conf_set_int(conf, CONF_port, port);
        else if (vt->default_port)
            conf_set_int(conf, CONF_port, vt->default_port);
        else
            err = dupprintf("A %s connection needs a port number: "
                            "host:port", vt->displayname_lc);
    }
    sfree(host);
    return err;
}

/* ----------------------------------------------------------------------
 * Folders.
 */

static int cmp_ci(const void *av, const void *bv)
{
    return _stricmp(*(const char *const *)av, *(const char *const *)bv);
}

char **folders_get(size_t *nout)
{
    char **f = NULL;
    size_t n = 0, size = 0;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, FOLDERS_KEY);
    if (key) {
        for (DWORD i = 0;; i++) {
            char name[1024];
            DWORD len = sizeof(name);
            LONG r = RegEnumValueA(key, i, name, &len, NULL, NULL, NULL,
                                   NULL);
            if (r == ERROR_MORE_DATA)
                continue;              /* (too long to be one of ours) */
            if (r != ERROR_SUCCESS)
                break;
            if (!*name)
                continue;
            sgrowarray(f, size, n);
            f[n++] = dupstr(name);
        }
        close_regkey(key);
    }
    /* (and PuTTY-ng's, unless they're here already) */
    struct sesslist sl;
    get_sesslist(&sl, true);
    for (int i = 0; i < sl.nsessions; i++) {
        const char *name = sl.sessions[i];
        size_t len = strlen(name);
        if (len < 2 || name[len - 1] != FOLDER_SEP)
            continue;
        char *path = dupprintf("%.*s", (int)(len - 1), name);
        bool seen = false;
        for (size_t j = 0; j < n && !seen; j++)
            seen = !strcmp(f[j], path);
        if (seen) {
            sfree(path);
            continue;
        }
        sgrowarray(f, size, n);
        f[n++] = path;
    }
    get_sesslist(&sl, false);
    if (n)
        qsort(f, n, sizeof(*f), cmp_ci);
    *nout = n;
    return f;
}

void folders_free(char **f, size_t n)
{
    for (size_t i = 0; i < n; i++)
        sfree(f[i]);
    sfree(f);
}

void folder_add(const char *path)
{
    put_info(FOLDERS_KEY, path, "");
}

/* Is 'name' 'path', or in it? */
static bool in_folder(const char *name, const char *path)
{
    size_t len = strlen(path);
    return !strncmp(name, path, len) &&
        (!name[len] || name[len] == FOLDER_SEP);
}

/* PuTTY-ng's folder, as a session, if there is one */
static bool ng_folder_exists(const char *path)
{
    char *name = dupcat(path, FOLDER_SEP_S);
    bool there = session_exists(name);
    sfree(name);
    return there;
}

void folder_remove(const char *path)
{
    size_t n;
    char **f = folders_get(&n);
    for (size_t i = 0; i < n; i++) {
        if (!in_folder(f[i], path))
            continue;
        put_info(FOLDERS_KEY, f[i], NULL);
        if (ng_folder_exists(f[i])) {
            char *name = dupcat(f[i], FOLDER_SEP_S);
            del_settings(name);
            sfree(name);
        }
    }
    folders_free(f, n);
}

void folder_rename(const char *from, const char *to)
{
    size_t n, len = strlen(from);
    char **f = folders_get(&n);
    for (size_t i = 0; i < n; i++) {
        if (!in_folder(f[i], from))
            continue;
        /* (to the top level, if 'to' is "") */
        const char *rest = f[i] + len;
        char *moved = *to ? dupcat(to, rest) :
            dupstr(rest + (*rest == FOLDER_SEP));
        put_info(FOLDERS_KEY, f[i], NULL);
        if (ng_folder_exists(f[i])) {
            /* (PuTTY-ng's goes too, where it would look for it) */
            char *oldname = dupcat(f[i], FOLDER_SEP_S);
            char *newname = dupcat(moved, FOLDER_SEP_S);
            char *err = *moved ? session_rename(oldname, newname) : NULL;
            if (!*moved)
                del_settings(oldname);
            sfree(err);
            sfree(newname);
            sfree(oldname);
        } else if (*moved) {
            put_info(FOLDERS_KEY, moved, "");
        }
        sfree(moved);
    }
    folders_free(f, n);
}

/* ----------------------------------------------------------------------
 * How the Home page was left.
 */

static unsigned get_flags(const char *name)
{
    DWORD val = 0;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, WINUI_KEY);
    if (key) {
        if (!get_reg_dword(key, name, &val))
            val = 0;
        close_regkey(key);
    }
    return val;
}

static void put_flags(const char *name, unsigned flags)
{
    HKEY key = create_regkey(HKEY_CURRENT_USER, WINUI_KEY);
    if (key) {
        put_reg_dword(key, name, flags);
        close_regkey(key);
    }
}

unsigned home_hidden_get(void)
{
    return get_flags("HiddenSections");
}

void home_hidden_set(unsigned hidden)
{
    put_flags("HiddenSections", hidden);
}

unsigned popup_omitted_get(void)
{
    return get_flags("PopupOmittedSections");
}

void popup_omitted_set(unsigned omitted)
{
    put_flags("PopupOmittedSections", omitted);
}

char **home_collapsed_get(size_t *nout)
{
    char **f = NULL;
    size_t n = 0, size = 0;
    HKEY key = open_regkey_ro(HKEY_CURRENT_USER, WINUI_KEY);
    strbuf *sb = key ? get_reg_multi_sz(key, "CollapsedFolders") : NULL;
    if (sb) {
        BinarySource src[1];
        BinarySource_BARE_INIT_PL(src, ptrlen_from_strbuf(sb));
        const char *folder;
        while (folder = get_asciz(src), !get_err(src)) {
            if (!*folder)
                continue;
            sgrowarray(f, size, n);
            f[n++] = dupstr(folder);
        }
        strbuf_free(sb);
    }
    if (key)
        close_regkey(key);
    *nout = n;
    return f;
}

void home_collapsed_set(char *const *folders, size_t n)
{
    HKEY key = create_regkey(HKEY_CURRENT_USER, WINUI_KEY);
    if (!key)
        return;
    if (n) {
        strbuf *sb = strbuf_new();
        for (size_t i = 0; i < n; i++)
            put_asciz(sb, folders[i]);
        put_reg_multi_sz(key, "CollapsedFolders", sb);
        strbuf_free(sb);
    } else {
        RegDeleteValueA(key, "CollapsedFolders");
    }
    close_regkey(key);
}

/* ----------------------------------------------------------------------
 * Exporting and importing, as .reg files, with Windows's reg.exe.
 */

static char *run_reg(const wchar_t *args)
{
    wchar_t sysdir[MAX_PATH];
    if (!GetSystemDirectoryW(sysdir, MAX_PATH))
        return dupstr("Couldn't find reg.exe");
    wchar_t *exe = dupwcscat(sysdir, L"\\reg.exe");
    wchar_t *cmd = dupwcscat(L"\"", exe, L"\" ", args);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    char *err = NULL;
    if (!CreateProcessW(exe, cmd, NULL, NULL, false, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        err = dupstr("Couldn't run reg.exe");
    } else {
        DWORD code = 1;
        if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0)
            err = dupstr("reg.exe took too long");
        else if (GetExitCodeProcess(pi.hProcess, &code) && code != 0)
            err = dupstr("reg.exe failed");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    sfree(cmd);
    sfree(exe);
    return err;
}

char *sessions_export(const char *name, const wchar_t *file)
{
    strbuf *key = strbuf_new();
    put_fmt(key, "HKCU\\%s", SESSIONS_KEY);
    if (name) {
        put_byte(key, '\\');
        escape_registry_key(name, key);
    }
    wchar_t *wkey = dup_mb_to_wc(CP_ACP, key->s);
    wchar_t *args = dupwcscat(L"export \"", wkey, L"\" \"", file,
                              L"\" /y");
    char *err = run_reg(args);
    sfree(args);
    sfree(wkey);
    strbuf_free(key);
    return err;
}

/*
 * A .reg file can change anything in the registry, so before letting
 * reg.exe at one, make sure it's just PuTTY sessions: that every key
 * it names is a session (with nothing deleted). Returns how many
 * sessions it has, or -1 if it's not one of ours.
 */
static int check_reg_file(const wchar_t *file)
{
    FILE *fp = _wfopen(file, L"rb");
    if (!fp)
        return -1;
    strbuf *raw = strbuf_new();
    char buf[4096];
    size_t len;
    while ((len = fread(buf, 1, sizeof(buf), fp)) > 0 && raw->len < 64 << 20)
        put_data(raw, buf, len);
    fclose(fp);

    /* (reg.exe writes UTF-16, but REGEDIT4 files are ANSI) */
    char *text;
    if (raw->len >= 2 && raw->u[0] == 0xFF && raw->u[1] == 0xFE) {
        size_t nw = (raw->len - 2) / 2;
        wchar_t *w = snewn(nw + 1, wchar_t);
        memcpy(w, raw->u + 2, nw * 2);
        w[nw] = L'\0';
        text = dup_wc_to_mb(CP_UTF8, w, "?");
        sfree(w);
    } else {
        text = dupstr(raw->s);
    }
    strbuf_free(raw);

    static const char prefix[] =
        "[HKEY_CURRENT_USER\\" SESSIONS_KEY "\\";
    int count = 0;
    bool ok = true, header = false;
    for (char *line = text; ok && *line; ) {
        char *end = line + strcspn(line, "\r\n");
        while (line < end && (*line == ' ' || *line == '\t'))
            line++;
        if (line < end && *line == '[') {
            size_t n = end - line;
            /* (exporting them all starts with the key they're in) */
            static const char parent[] =
                "[HKEY_CURRENT_USER\\" SESSIONS_KEY "]";
            if (n == sizeof(parent) - 1 && !strncmp(line, parent, n)) {
                line = end + strspn(end, "\r\n");
                continue;
            }
            ok = n > sizeof(prefix) - 1 && !strncmp(line, prefix,
                                                    sizeof(prefix) - 1) &&
                !memchr(line + sizeof(prefix) - 1, '\\',
                        n - (sizeof(prefix) - 1));
            count++;
        } else if (line < end && !header) {
            ok = !strncmp(line, "Windows Registry Editor", 23) ||
                !strncmp(line, "REGEDIT", 7);
            header = true;
        }
        line = end + strspn(end, "\r\n");
    }
    sfree(text);
    return ok && header ? count : -1;
}

char *sessions_import(const wchar_t *file, int *count)
{
    int n = check_reg_file(file);
    if (n < 0)
        return dupstr("That file isn't just PuTTY sessions (as exported "
                      "from PuTTY), so it hasn't been imported.");
    *count = n;
    wchar_t *args = dupwcscat(L"import \"", file, L"\"");
    char *err = run_reg(args);
    sfree(args);
    return err;
}

/* ----------------------------------------------------------------------
 * Comparing settings.
 */

bool conf_key_equal(Conf *a, Conf *b, int key)
{
    switch (conf_key_info[key].value_type) {
      case CONF_TYPE_BOOL:
        return conf_get_bool(a, key) == conf_get_bool(b, key);
      case CONF_TYPE_INT:
        return conf_get_int(a, key) == conf_get_int(b, key);
      case CONF_TYPE_STR:
      case CONF_TYPE_UTF8:
      case CONF_TYPE_STR_AMBI: {
        bool ua, ub;
        const char *sa = conf_get_str_ambi(a, key, &ua);
        const char *sb = conf_get_str_ambi(b, key, &ub);
        return ua == ub && !strcmp(sa, sb);
      }
      case CONF_TYPE_FILENAME:
        return filename_equal(conf_get_filename(a, key),
                              conf_get_filename(b, key));
      case CONF_TYPE_FONT: {
        FontSpec *fa = conf_get_fontspec(a, key);
        FontSpec *fb = conf_get_fontspec(b, key);
        return !strcmp(fa->name, fb->name) && fa->isbold == fb->isbold &&
            fa->height == fb->height && fa->charset == fb->charset;
      }
      default:
        return true;                   /* (not one we can tell) */
    }
}

void conf_key_copy(Conf *to, Conf *from, int key)
{
    switch (conf_key_info[key].value_type) {
      case CONF_TYPE_BOOL:
        conf_set_bool(to, key, conf_get_bool(from, key));
        break;
      case CONF_TYPE_INT:
        conf_set_int(to, key, conf_get_int(from, key));
        break;
      case CONF_TYPE_STR:
        conf_set_str(to, key, conf_get_str(from, key));
        break;
      case CONF_TYPE_UTF8:
        conf_set_utf8(to, key, conf_get_utf8(from, key));
        break;
      case CONF_TYPE_STR_AMBI: {
        bool utf8;
        const char *s = conf_get_str_ambi(from, key, &utf8);
        if (utf8)
            conf_set_utf8(to, key, s);
        else
            conf_set_str(to, key, s);
        break;
      }
      case CONF_TYPE_FILENAME:
        conf_set_filename(to, key, conf_get_filename(from, key));
        break;
      case CONF_TYPE_FONT:
        conf_set_fontspec(to, key, conf_get_fontspec(from, key));
        break;
    }
}

bool conf_equal(Conf *a, Conf *b)
{
    strbuf *sa = strbuf_new(), *sb = strbuf_new();
    conf_serialise(BinarySink_UPCAST(sa), a);
    conf_serialise(BinarySink_UPCAST(sb), b);
    bool eq = sa->len == sb->len && !memcmp(sa->u, sb->u, sa->len);
    strbuf_free(sa);
    strbuf_free(sb);
    return eq;
}
