/*
 * autologon.h: the steps of Automate logon, as kept in CONF_autologon,
 * shared by the config box, which edits them, and the front end,
 * which carries them out.
 *
 * The keys are the steps' numbers, as three decimal digits, so that
 * they sort in order. Each value is some flags ('D' if the step is
 * switched off, 'H' to hide the command, 'C' if the command is really
 * in the Windows Credential Manager), a newline, the text to wait for,
 * another newline, and the command (or for 'C', the name of the
 * credential it's in).
 *
 * Hidden commands (passwords, mostly) aren't saved with the rest of
 * a session's settings: the config box puts them in the Credential
 * Manager when it saves the session, under a name made from the
 * session's, and the Conf has 'C' steps naming them. In a Conf that
 * hasn't come from a saved session (or has been edited since), the
 * commands are just there, as for any other step.
 */

#ifndef PUTTY_WINUI_AUTOLOGON_H
#define PUTTY_WINUI_AUTOLOGON_H

#include <wincred.h>

typedef struct AutologonStep {
    bool active;               /* switched on */
    char *expect;              /* text to wait for; empty for none */
    char *command;             /* what to send then (followed by Enter) */
    bool hide;                 /* don't show the command's echo */
    bool stored;               /* the command is a credential's name */
} AutologonStep;

/* All the steps, in order */
static inline AutologonStep *autologon_get_steps(Conf *conf, size_t *nsteps)
{
    AutologonStep *steps = NULL;
    size_t n = 0, size = 0;
    char *key;
    const char *val;

    for (val = conf_get_str_strs(conf, CONF_autologon, NULL, &key);
         val != NULL;
         val = conf_get_str_strs(conf, CONF_autologon, key, &key)) {
        const char *nl1 = strchr(val, '\n');
        const char *nl2 = nl1 ? strchr(nl1 + 1, '\n') : NULL;
        if (!nl2)
            continue;                  /* not one of ours */
        sgrowarray(steps, size, n);
        steps[n].active = !memchr(val, 'D', nl1 - val);
        steps[n].hide = memchr(val, 'H', nl1 - val) != NULL;
        steps[n].stored = memchr(val, 'C', nl1 - val) != NULL;
        steps[n].expect = dupprintf("%.*s", (int)(nl2 - (nl1 + 1)), nl1 + 1);
        steps[n].command = dupstr(nl2 + 1);
        n++;
    }
    *nsteps = n;
    return steps;
}

static inline void autologon_free_steps(AutologonStep *steps, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sfree(steps[i].expect);
        burnstr(steps[i].command);
    }
    sfree(steps);
}

/* Replace all the steps */
static inline void autologon_set_steps(Conf *conf, const AutologonStep *steps,
                                       size_t n)
{
    char *key;
    while ((key = conf_get_str_nthstrkey(conf, CONF_autologon, 0)) != NULL)
        conf_del_str_str(conf, CONF_autologon, key);
    for (size_t i = 0; i < n; i++) {
        char keybuf[16];
        snprintf(keybuf, sizeof(keybuf), "%03u", (unsigned)i);
        char *val = dupprintf("%s%s%s\n%s\n%s", steps[i].active ? "" : "D",
                              steps[i].hide ? "H" : "",
                              steps[i].stored ? "C" : "",
                              steps[i].expect, steps[i].command);
        conf_set_str_str(conf, CONF_autologon, keybuf, val);
        burnstr(val);
    }
}

/* ----------------------------------------------------------------------
 * The Credential Manager. The credentials are generic ones, named
 * "PuTTY Automate logon/<session>/<step>", holding the command as is.
 */

#define AUTOLOGON_CRED_PREFIX "PuTTY Automate logon/"

static inline char *autologon_cred_name(const char *session, size_t step)
{
    return dupprintf(AUTOLOGON_CRED_PREFIX "%s/%u",
                     *session ? session : "Default Settings",
                     (unsigned)step);
}

static inline bool autologon_cred_write(const char *name, const char *secret)
{
    wchar_t *wname = dup_mb_to_wc(CP_ACP, name);
    CREDENTIALW cred;
    memset(&cred, 0, sizeof(cred));
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = wname;
    cred.Comment = L"A command sent by PuTTY's Automate logon";
    cred.CredentialBlobSize = (DWORD)strlen(secret);
    cred.CredentialBlob = (LPBYTE)secret;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = L"PuTTY";
    bool ok = CredWriteW(&cred, 0);
    sfree(wname);
    return ok;
}

/* The command in a credential, or NULL if there's no such credential */
static inline char *autologon_cred_read(const char *name)
{
    wchar_t *wname = dup_mb_to_wc(CP_ACP, name);
    PCREDENTIALW cred = NULL;
    char *ret = NULL;
    if (CredReadW(wname, CRED_TYPE_GENERIC, 0, &cred)) {
        ret = snewn(cred->CredentialBlobSize + 1, char);
        memcpy(ret, cred->CredentialBlob, cred->CredentialBlobSize);
        ret[cred->CredentialBlobSize] = '\0';
        smemclr(cred->CredentialBlob, cred->CredentialBlobSize);
        CredFree(cred);
    }
    sfree(wname);
    return ret;
}

/* Remove the credentials whose names match 'filter' (with '*' at the
 * end, as CredEnumerate takes it) */
static inline void autologon_cred_delete_matching(const char *filter)
{
    wchar_t *wfilter = dup_mb_to_wc(CP_ACP, filter);
    DWORD count = 0;
    PCREDENTIALW *creds = NULL;
    if (CredEnumerateW(wfilter, 0, &count, &creds)) {
        for (DWORD i = 0; i < count; i++)
            CredDeleteW(creds[i]->TargetName, CRED_TYPE_GENERIC, 0);
        CredFree(creds);
    }
    sfree(wfilter);
}

/* Remove all of a session's credentials */
static inline void autologon_cred_delete_session(const char *session)
{
    char *filter = dupprintf(AUTOLOGON_CRED_PREFIX "%s/*",
                             *session ? session : "Default Settings");
    autologon_cred_delete_matching(filter);
    sfree(filter);
}

/* ... or every session's (for -cleanup) */
static inline void autologon_cred_delete_all(void)
{
    autologon_cred_delete_matching(AUTOLOGON_CRED_PREFIX "*");
}

/* A step's command, wherever it is (NULL if its credential has gone) */
static inline char *autologon_command(const AutologonStep *step)
{
    return step->stored ? autologon_cred_read(step->command) :
        dupstr(step->command);
}

#endif /* PUTTY_WINUI_AUTOLOGON_H */
