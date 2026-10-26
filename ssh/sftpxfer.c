/*
 * Transferring files by SFTP, over a channel of its own in an SSH
 * session that's already running: for the GUI front end's 'Send
 * File' and 'Receive File'. Each job (finding the home directory,
 * listing a directory, checking a file is there, sending or receiving
 * a file) opens its own channel and closes it when it's done, so that
 * nothing's left open to keep the connection alive once the main
 * session has finished.
 *
 * This is all event-driven, unlike the SFTP client in sftp.c, which
 * psftp and pscp use, and which blocks while waiting for replies.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "putty.h"
#include "ssh.h"
#include "channel.h"
#include "sftp.h"

#define XFER_BLOCK 32768               /* data in each WRITE or READ */
#define XFER_MAX_REQS 16               /* WRITEs or READs at once */
#define XFER_MAX_PACKET (256 * 1024)   /* largest reply we'll take */
#define XFER_MAX_ENTRIES 5000          /* most of a directory we'll list */

typedef enum XferJob {
    JOB_HOME, JOB_SEND, JOB_RECEIVE, JOB_CHECK, JOB_LIST
} XferJob;

typedef enum XferState {
    XS_OPENING,                        /* waiting for the channel */
    XS_SUBSYSTEM,                      /* ... and the sftp subsystem */
    XS_INIT,                           /* ... and SSH_FXP_VERSION */
    XS_REALPATH,                       /* REALPATH of "." (home), or of
                                        * the directory or the file */
    XS_STATDIR,                        /* STAT of where to send it */
    XS_STAT,                           /* STAT of the file */
    XS_OPEN,                           /* OPEN of the file */
    XS_TRANSFER,                       /* WRITEs or READs */
    XS_OPENDIR,                        /* OPENDIR of the directory */
    XS_READDIR,                        /* READDIRs */
    XS_CLOSING,                        /* CLOSE of the handle */
    XS_DONE
} XferState;

/* A WRITE or READ awaiting its reply */
typedef struct XferReq {
    unsigned id;
    uint64_t offset;
    size_t len;
    strbuf *data;                      /* (READ: what came back) */
    bool replied, at_eof;              /* (READ: it came back, or EOF) */
    bool discard;                      /* (READ: not wanted after all) */
} XferReq;

struct SftpXfer {
    const SftpXferClient *client;      /* NULL once it's been told */
    void *ctx;
    ConnectionLayer *cl;
    SshChannel *sc;
    bool closing;                      /* channel closed or closing */

    XferJob job;
    XferState state;
    char *dir, *name, *path;           /* (sending: path = dir/name) */
    bool overwrite;

    FILE *fp;                          /* (sending: from; receiving: to) */
    uint64_t size, offset, done;       /* (offset: of the next request) */
    bool eof, input_wanted;
    strbuf *handle;
    int nentries;

    bufchain inbuf;
    unsigned req_id, next_id;          /* the request we want a reply to */
    XferReq reqs[2 * XFER_MAX_REQS];   /* (room for discarded ones) */
    int nreqs;

    Channel chan;
};

static void xfer_chan_free(Channel *chan);
static void xfer_open_confirmation(Channel *chan);
static void xfer_open_failed(Channel *chan, const char *err);
static size_t xfer_send(Channel *chan, bool is_stderr,
                        const void *data, size_t len);
static void xfer_send_eof(Channel *chan);
static void xfer_set_input_wanted(Channel *chan, bool wanted);
static char *xfer_log_close_msg(Channel *chan);
static void xfer_request_response(Channel *chan, bool success);

static const ChannelVtable xfer_channelvt = {
    .free = xfer_chan_free,
    .open_confirmation = xfer_open_confirmation,
    .open_failed = xfer_open_failed,
    .send = xfer_send,
    .send_eof = xfer_send_eof,
    .set_input_wanted = xfer_set_input_wanted,
    .log_close_msg = xfer_log_close_msg,
    .want_close = chan_default_want_close,
    .rcvd_exit_status = chan_no_exit_status,
    .rcvd_exit_signal = chan_no_exit_signal,
    .rcvd_exit_signal_numeric = chan_no_exit_signal_numeric,
    .run_shell = chan_no_run_shell,
    .run_command = chan_no_run_command,
    .run_subsystem = chan_no_run_subsystem,
    .enable_x11_forwarding = chan_no_enable_x11_forwarding,
    .enable_agent_forwarding = chan_no_enable_agent_forwarding,
    .allocate_pty = chan_no_allocate_pty,
    .set_env = chan_no_set_env,
    .send_break = chan_no_send_break,
    .send_signal = chan_no_send_signal,
    .change_window_size = chan_no_change_window_size,
    .request_response = xfer_request_response,
};

/* ----------------------------------------------------------------------
 * Finishing, one way or another.
 */

static void xfer_close_callback(void *vctx)
{
    SftpXfer *x = (SftpXfer *)vctx;
    sshfwd_initiate_close(x->sc, NULL); /* (which frees x) */
}

/*
 * Tell the client how it went, and close the channel. (Not right
 * away, since we might be deep in the connection layer's handling of
 * something that came in on it.)
 */
static void xfer_finish(SftpXfer *x, SftpXferResult result, const char *text)
{
    if (x->fp) {
        fclose(x->fp);
        x->fp = NULL;
    }
    x->state = XS_DONE;
    if (x->client) {
        const SftpXferClient *client = x->client;
        x->client = NULL;
        client->done(x->ctx, result, text);
    }
    if (!x->closing) {
        x->closing = true;
        queue_toplevel_callback(xfer_close_callback, x);
    }
}

static PRINTF_LIKE(2, 3) void xfer_fail(SftpXfer *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = dupvprintf(fmt, ap);
    va_end(ap);
    if (x->job == JOB_SEND || x->job == JOB_RECEIVE)
        logeventf(x->cl->logctx, "%s file: %s",
                  x->job == JOB_SEND ? "Send" : "Receive", msg);
    xfer_finish(x, SFTP_XFER_FAILED, msg);
    sfree(msg);
}

void sftp_xfer_cancel(SftpXfer *x)
{
    x->client = NULL;
    if (x->fp) {
        fclose(x->fp);
        x->fp = NULL;
    }
    if (!x->closing) {
        x->closing = true;
        sshfwd_initiate_close(x->sc, NULL); /* (which frees x) */
    }
}

static void xfer_chan_free(Channel *chan)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    x->closing = true;                 /* (so xfer_finish won't) */
    if (x->client)                     /* (e.g. the connection's gone) */
        xfer_finish(x, SFTP_XFER_FAILED, "The connection was closed");
    if (x->fp)
        fclose(x->fp);
    delete_callbacks_for_context(x);
    bufchain_clear(&x->inbuf);
    if (x->handle)
        strbuf_free(x->handle);
    for (int i = 0; i < x->nreqs; i++)
        if (x->reqs[i].data)
            strbuf_free(x->reqs[i].data);
    sfree(x->dir);
    sfree(x->name);
    sfree(x->path);
    sfree(x);
}

/* ----------------------------------------------------------------------
 * Sending SFTP packets.
 */

static strbuf *xfer_packet(int type)
{
    strbuf *pkt = strbuf_new();
    put_uint32(pkt, 0);                /* (the length, filled in later) */
    put_byte(pkt, type);
    return pkt;
}

/* A request whose reply we'll wait for */
static strbuf *xfer_request(SftpXfer *x, int type)
{
    strbuf *pkt = xfer_packet(type);
    x->req_id = ++x->next_id;
    put_uint32(pkt, x->req_id);
    return pkt;
}

static void xfer_send_packet(SftpXfer *x, strbuf *pkt)
{
    PUT_32BIT_MSB_FIRST(pkt->u, pkt->len - 4);
    sshfwd_write(x->sc, pkt->u, pkt->len);
    strbuf_free(pkt);
}

static void xfer_send_path_request(SftpXfer *x, int type, const char *path,
                                   XferState state)
{
    strbuf *pkt = xfer_request(x, type);
    put_stringz(pkt, path);
    xfer_send_packet(x, pkt);
    x->state = state;
}

static void xfer_send_handle_request(SftpXfer *x, int type, XferState state)
{
    strbuf *pkt = xfer_request(x, type);
    put_stringpl(pkt, ptrlen_from_strbuf(x->handle));
    xfer_send_packet(x, pkt);
    x->state = state;
}

/* ----------------------------------------------------------------------
 * Reading SFTP replies.
 */

typedef struct XferAttrs {
    bool have_size, have_perms;
    uint64_t size;
    unsigned long perms;
} XferAttrs;

static bool get_attrs(BinarySource *src, XferAttrs *a)
{
    memset(a, 0, sizeof(*a));
    unsigned long flags = get_uint32(src);
    if (flags & SSH_FILEXFER_ATTR_SIZE) {
        a->have_size = true;
        a->size = get_uint64(src);
    }
    if (flags & SSH_FILEXFER_ATTR_UIDGID) {
        get_uint32(src);
        get_uint32(src);
    }
    if (flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
        a->have_perms = true;
        a->perms = get_uint32(src);
    }
    if (flags & SSH_FILEXFER_ATTR_ACMODTIME) {
        get_uint32(src);
        get_uint32(src);
    }
    if (flags & SSH_FILEXFER_ATTR_EXTENDED) {
        unsigned long n = get_uint32(src);
        for (unsigned long i = 0; i < n && !get_err(src); i++) {
            get_string(src);
            get_string(src);
        }
    }
    return !get_err(src);
}

static bool attrs_dir(const XferAttrs *a)
{
    return a->have_perms && (a->perms & 0170000) == PERMS_DIRECTORY;
}

static const char *status_text(unsigned code)
{
    switch (code) {
      case SSH_FX_EOF: return "end of file";
      case SSH_FX_NO_SUCH_FILE: return "no such file or directory";
      case SSH_FX_PERMISSION_DENIED: return "permission denied";
      case SSH_FX_BAD_MESSAGE: return "bad message";
      case SSH_FX_NO_CONNECTION: return "no connection";
      case SSH_FX_CONNECTION_LOST: return "connection lost";
      case SSH_FX_OP_UNSUPPORTED: return "operation unsupported";
      default: return "failure";
    }
}

/* What went wrong with a request whose reply is an SSH_FXP_STATUS */
static void xfer_status_fail(SftpXfer *x, const char *what, unsigned code,
                             ptrlen msg)
{
    if (msg.len)
        xfer_fail(x, "%s: %.*s", what, PTRLEN_PRINTF(msg));
    else
        xfer_fail(x, "%s: %s", what, status_text(code));
}

/* ----------------------------------------------------------------------
 * The steps of the jobs.
 */

/*
 * The server works out where a path is. A relative one is from the
 * home directory, so we can deal with a leading ~ (which SFTP doesn't
 * know about) by just leaving it out.
 */
static char *untilde(const char *path)
{
    if (path[0] == '~' && (path[1] == '/' || !path[1]))
        return dupcat(".", path + 1);
    return dupstr(*path ? path : ".");
}

static void xfer_start(SftpXfer *x)
{
    char *path;
    switch (x->job) {
      case JOB_HOME:
        xfer_send_path_request(x, SSH_FXP_REALPATH, ".", XS_REALPATH);
        break;
      case JOB_SEND:
      case JOB_LIST:
        path = untilde(x->dir);
        xfer_send_path_request(x, SSH_FXP_REALPATH, path, XS_REALPATH);
        sfree(path);
        break;
      case JOB_CHECK:
        path = untilde(x->path);
        xfer_send_path_request(x, SSH_FXP_REALPATH, path, XS_REALPATH);
        sfree(path);
        break;
      case JOB_RECEIVE:
        path = untilde(x->path);
        sfree(x->path);
        x->path = path;
        xfer_send_path_request(x, SSH_FXP_STAT, x->path, XS_STAT);
        break;
    }
}

static void xfer_open_file(SftpXfer *x)
{
    logeventf(x->cl->logctx, x->job == JOB_SEND ?
              "Send file: sending to %s" : "Receive file: receiving %s",
              x->path);
    strbuf *pkt = xfer_request(x, SSH_FXP_OPEN);
    put_stringz(pkt, x->path);
    put_uint32(pkt, x->job == JOB_SEND ?
               SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC : SSH_FXF_READ);
    put_uint32(pkt, 0);                /* no attributes */
    xfer_send_packet(x, pkt);
    x->state = XS_OPEN;
}

static int live_reqs(SftpXfer *x)
{
    int n = 0;
    for (int i = 0; i < x->nreqs; i++)
        if (!x->reqs[i].discard)
            n++;
    return n;
}

static void remove_req(SftpXfer *x, int i)
{
    if (x->reqs[i].data)
        strbuf_free(x->reqs[i].data);
    memmove(x->reqs + i, x->reqs + i + 1,
            (x->nreqs - i - 1) * sizeof(*x->reqs));
    x->nreqs--;
}

static XferReq *add_req(SftpXfer *x, uint64_t offset, size_t len)
{
    assert(x->nreqs < lenof(x->reqs));
    XferReq *r = &x->reqs[x->nreqs++];
    memset(r, 0, sizeof(*r));
    r->id = ++x->next_id;
    r->offset = offset;
    r->len = len;
    return r;
}

/* Sending: keep enough WRITEs on their way, and close the file when
 * done */
static void xfer_pump_writes(SftpXfer *x)
{
    char buf[XFER_BLOCK];

    while (x->state == XS_TRANSFER && x->input_wanted && !x->eof &&
           x->nreqs < XFER_MAX_REQS) {
        size_t len = fread(buf, 1, sizeof(buf), x->fp);
        if (len == 0) {
            if (ferror(x->fp)) {
                xfer_fail(x, "Couldn't read the local file");
                return;
            }
            x->eof = true;
            break;
        }

        XferReq *r = add_req(x, x->offset, len);
        strbuf *pkt = xfer_packet(SSH_FXP_WRITE);
        put_uint32(pkt, r->id);
        put_stringpl(pkt, ptrlen_from_strbuf(x->handle));
        put_uint64(pkt, x->offset);
        put_string(pkt, buf, len);
        xfer_send_packet(x, pkt);
        x->offset += len;
    }

    if (x->state == XS_TRANSFER && x->eof && x->nreqs == 0)
        xfer_send_handle_request(x, SSH_FXP_CLOSE, XS_CLOSING);
}

/*
 * Receiving: write out what's come back, in order; keep enough READs
 * on their way; and close the file when it's all here. A short read
 * (which the server may give us even before the end of the file)
 * means that the READs after it are for the wrong places, so we
 * ignore them and carry on from where it stopped.
 */
static void xfer_pump_reads(SftpXfer *x)
{
    while (x->state == XS_TRANSFER) {
        int i;
        for (i = 0; i < x->nreqs && x->reqs[i].discard; i++);
        if (i == x->nreqs || !x->reqs[i].replied)
            break;
        XferReq *r = &x->reqs[i];
        bool short_read = false;
        if (r->at_eof) {
            x->eof = true;
            short_read = true;
        } else {
            size_t len = r->data->len;
            if (fwrite(r->data->u, 1, len, x->fp) != len) {
                xfer_fail(x, "Couldn't write the local file");
                return;
            }
            x->done += len;
            if (len < r->len) {
                x->offset = r->offset + len;
                short_read = true;
            }
        }
        remove_req(x, i);
        if (short_read)
            for (int j = 0; j < x->nreqs; j++)
                x->reqs[j].discard = true;
    }
    if (x->state != XS_TRANSFER)
        return;

    /* (we ask for one block more than the file's size, to find out
     * that it ends there, or else that it's grown, in which case we
     * keep going one at a time) */
    while (!x->eof && x->input_wanted && x->nreqs < lenof(x->reqs) &&
           live_reqs(x) < XFER_MAX_REQS &&
           (x->offset <= x->size || live_reqs(x) == 0)) {
        XferReq *r = add_req(x, x->offset, XFER_BLOCK);
        strbuf *pkt = xfer_packet(SSH_FXP_READ);
        put_uint32(pkt, r->id);
        put_stringpl(pkt, ptrlen_from_strbuf(x->handle));
        put_uint64(pkt, x->offset);
        put_uint32(pkt, XFER_BLOCK);
        xfer_send_packet(x, pkt);
        x->offset += XFER_BLOCK;
    }

    if (x->eof && live_reqs(x) == 0)
        xfer_send_handle_request(x, SSH_FXP_CLOSE, XS_CLOSING);
}

static void xfer_progress(SftpXfer *x)
{
    if (x->client && x->client->progress)
        x->client->progress(x->ctx, x->done, x->size);
}

/* A reply to a WRITE or READ */
static void xfer_transfer_reply(SftpXfer *x, int i, int type,
                                BinarySource *src, unsigned code, ptrlen msg)
{
    XferReq *r = &x->reqs[i];
    if (r->discard) {
        remove_req(x, i);
        return;
    }

    if (x->job == JOB_SEND) {
        if (type != SSH_FXP_STATUS) {
            xfer_fail(x, "Unexpected reply from the server");
            return;
        }
        if (code != SSH_FX_OK) {
            xfer_status_fail(x, x->path, code, msg);
            return;
        }
        x->done += r->len;
        remove_req(x, i);
        xfer_progress(x);
        xfer_pump_writes(x);
        return;
    }

    if (type == SSH_FXP_STATUS) {
        if (code != SSH_FX_EOF) {
            xfer_status_fail(x, x->path, code, msg);
            return;
        }
        r->at_eof = true;
    } else if (type == SSH_FXP_DATA) {
        ptrlen data = get_string(src);
        if (get_err(src) || data.len > r->len) {
            xfer_fail(x, "Unexpected reply from the server");
            return;
        }
        r->data = strbuf_dup(data);
    } else {
        xfer_fail(x, "Unexpected reply from the server");
        return;
    }
    r->replied = true;
    xfer_pump_reads(x);
    if (x->state == XS_TRANSFER || x->state == XS_CLOSING)
        xfer_progress(x);
}

static void xfer_packet_in(SftpXfer *x, int type, BinarySource *src)
{
    unsigned id = 0, code = SSH_FX_OK;
    ptrlen msg = PTRLEN_LITERAL("");
    XferAttrs attrs;

    if (x->state == XS_INIT) {
        if (type != SSH_FXP_VERSION) {
            xfer_fail(x, "The server didn't start SFTP");
            return;
        }
        xfer_start(x);
        return;
    }

    id = get_uint32(src);
    if (type == SSH_FXP_STATUS) {
        code = get_uint32(src);
        msg = get_string(src);         /* (absent before version 3) */
    }

    /* Replies to WRITEs and READs (including ones we no longer want,
     * which can still come in after we've stopped transferring) */
    for (int i = 0; i < x->nreqs; i++) {
        if (x->reqs[i].id == id) {
            xfer_transfer_reply(x, i, type, src, code, msg);
            return;
        }
    }

    if (id != x->req_id) {
        xfer_fail(x, "Unexpected reply from the server");
        return;
    }

    switch (x->state) {
      case XS_REALPATH: {
        if (type == SSH_FXP_STATUS) {
            if (code == SSH_FX_NO_SUCH_FILE && x->job != JOB_HOME)
                xfer_fail(x, "%s: no such %s",
                          x->job == JOB_CHECK ? x->path : x->dir,
                          x->job == JOB_CHECK ? "file" : "directory");
            else
                xfer_status_fail(x, x->job == JOB_HOME ? "Home directory" :
                                 x->job == JOB_CHECK ? x->path : x->dir,
                                 code, msg);
            return;
        }
        if (type != SSH_FXP_NAME || get_uint32(src) < 1)
            goto unexpected;
        ptrlen name = get_string(src);
        if (get_err(src))
            goto unexpected;
        char *path = mkstr(name);
        switch (x->job) {
          case JOB_HOME:
            xfer_finish(x, SFTP_XFER_OK, path);
            sfree(path);
            return;
          case JOB_SEND:
            sfree(x->dir);
            x->dir = path;
            xfer_send_path_request(x, SSH_FXP_STAT, x->dir, XS_STATDIR);
            return;
          case JOB_LIST:
            sfree(x->dir);
            x->dir = path;
            xfer_send_path_request(x, SSH_FXP_OPENDIR, x->dir, XS_OPENDIR);
            return;
          default:                     /* JOB_CHECK */
            sfree(x->path);
            x->path = path;
            xfer_send_path_request(x, SSH_FXP_STAT, x->path, XS_STAT);
            return;
        }
      }

      case XS_STATDIR:
        if (type == SSH_FXP_STATUS) {
            xfer_status_fail(x, x->dir, code, msg);
            return;
        }
        if (type != SSH_FXP_ATTRS || !get_attrs(src, &attrs))
            goto unexpected;
        if (!attrs_dir(&attrs) && attrs.have_perms) {
            xfer_fail(x, "%s: not a directory", x->dir);
            return;
        }
        sfree(x->path);
        x->path = dupcat(x->dir, (*x->dir && x->dir[strlen(x->dir) - 1]
                                  == '/') ? "" : "/", x->name);
        xfer_send_path_request(x, SSH_FXP_STAT, x->path, XS_STAT);
        return;

      case XS_STAT:
        if (type == SSH_FXP_STATUS) {
            if (x->job == JOB_SEND && code == SSH_FX_NO_SUCH_FILE)
                xfer_open_file(x);     /* (it's not there yet) */
            else if (code == SSH_FX_NO_SUCH_FILE)
                xfer_fail(x, "%s: no such file", x->path);
            else
                xfer_status_fail(x, x->path, code, msg);
            return;
        }
        if (type != SSH_FXP_ATTRS || !get_attrs(src, &attrs))
            goto unexpected;
        if (attrs_dir(&attrs)) {
            xfer_fail(x, "%s: is a directory", x->path);
            return;
        }
        switch (x->job) {
          case JOB_SEND:
            if (!x->overwrite)
                xfer_finish(x, SFTP_XFER_EXISTS, x->path);
            else
                xfer_open_file(x);
            return;
          case JOB_CHECK:
            xfer_finish(x, SFTP_XFER_OK, x->path);
            return;
          default:                     /* JOB_RECEIVE */
            x->size = attrs.have_size ? attrs.size : 0;
            xfer_open_file(x);
            return;
        }

      case XS_OPEN:
      case XS_OPENDIR: {
        if (type == SSH_FXP_STATUS) {
            if (x->state == XS_OPENDIR && code == SSH_FX_NO_SUCH_FILE)
                xfer_fail(x, "%s: no such directory", x->dir);
            else
                xfer_status_fail(x, x->state == XS_OPEN ? x->path : x->dir,
                                 code, msg);
            return;
        }
        if (type != SSH_FXP_HANDLE)
            goto unexpected;
        ptrlen handle = get_string(src);
        if (get_err(src))
            goto unexpected;
        x->handle = strbuf_dup(handle);
        if (x->state == XS_OPENDIR) {
            xfer_send_handle_request(x, SSH_FXP_READDIR, XS_READDIR);
            return;
        }
        x->state = XS_TRANSFER;
        if (x->job == JOB_SEND)
            xfer_pump_writes(x);
        else
            xfer_pump_reads(x);
        return;
      }

      case XS_READDIR:
        if (type == SSH_FXP_STATUS) {
            if (code != SSH_FX_EOF) {
                xfer_status_fail(x, x->dir, code, msg);
                return;
            }
            xfer_send_handle_request(x, SSH_FXP_CLOSE, XS_CLOSING);
            return;
        }
        if (type != SSH_FXP_NAME)
            goto unexpected;
        for (unsigned long n = get_uint32(src); n > 0; n--) {
            ptrlen name = get_string(src);
            get_string(src);           /* longname */
            if (!get_attrs(src, &attrs))
                goto unexpected;
            if (ptrlen_eq_string(name, ".") || ptrlen_eq_string(name, ".."))
                continue;
            if (x->client && x->client->entry) {
                char *s = mkstr(name);
                x->client->entry(x->ctx, s, attrs_dir(&attrs));
                sfree(s);
            }
            x->nentries++;
        }
        xfer_send_handle_request(x, x->nentries < XFER_MAX_ENTRIES ?
                                 SSH_FXP_READDIR : SSH_FXP_CLOSE,
                                 x->nentries < XFER_MAX_ENTRIES ?
                                 XS_READDIR : XS_CLOSING);
        return;

      case XS_CLOSING:
        if (type != SSH_FXP_STATUS)
            goto unexpected;
        if (x->job == JOB_LIST) {
            xfer_finish(x, SFTP_XFER_OK, x->dir);
            return;
        }
        if (code != SSH_FX_OK && x->job == JOB_SEND) {
            xfer_status_fail(x, x->path, code, msg);
            return;
        }
        if (x->job == JOB_RECEIVE && fflush(x->fp)) {
            xfer_fail(x, "Couldn't write the local file");
            return;
        }
        logeventf(x->cl->logctx, x->job == JOB_SEND ?
                  "Send file: sent %s" : "Receive file: received %s",
                  x->path);
        xfer_finish(x, SFTP_XFER_OK, x->path);
        return;

      default:
        break;
    }

  unexpected:
    xfer_fail(x, "Unexpected reply from the server");
}

/* ----------------------------------------------------------------------
 * The channel.
 */

static void xfer_open_confirmation(Channel *chan)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    if (!sshfwd_start_subsystem(x->sc, true, "sftp")) {
        xfer_fail(x, "SFTP needs SSH-2");
        return;
    }
    x->state = XS_SUBSYSTEM;
}

static void xfer_open_failed(Channel *chan, const char *err)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    x->closing = true;                 /* (it'll be freed separately) */
    xfer_fail(x, "Couldn't open a channel: %s", err);
}

static void xfer_request_response(Channel *chan, bool success)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    if (x->state != XS_SUBSYSTEM)
        return;
    if (!success) {
        xfer_fail(x, "The server refused to start SFTP");
        return;
    }
    strbuf *pkt = xfer_packet(SSH_FXP_INIT);
    put_uint32(pkt, SFTP_PROTO_VERSION);
    xfer_send_packet(x, pkt);
    x->state = XS_INIT;
}

static size_t xfer_send(Channel *chan, bool is_stderr,
                        const void *data, size_t len)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    if (is_stderr || x->state == XS_DONE)
        return 0;                      /* (ignored) */

    bufchain_add(&x->inbuf, data, len);
    while (x->state != XS_DONE && bufchain_size(&x->inbuf) >= 4) {
        unsigned char lenbuf[4];
        bufchain_fetch(&x->inbuf, lenbuf, 4);
        size_t pktlen = GET_32BIT_MSB_FIRST(lenbuf);
        if (pktlen < 1 || pktlen > XFER_MAX_PACKET) {
            xfer_fail(x, "Bad packet from the server");
            break;
        }
        if (bufchain_size(&x->inbuf) < 4 + pktlen)
            break;
        bufchain_consume(&x->inbuf, 4);
        unsigned char *pkt = snewn(pktlen, unsigned char);
        bufchain_fetch_consume(&x->inbuf, pkt, pktlen);
        BinarySource src[1];
        BinarySource_BARE_INIT(src, pkt + 1, pktlen - 1);
        xfer_packet_in(x, pkt[0], src);
        sfree(pkt);
    }
    return 0;
}

static void xfer_send_eof(Channel *chan)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    if (x->state != XS_DONE)
        xfer_fail(x, "The server closed the SFTP channel");
}

static void xfer_set_input_wanted(Channel *chan, bool wanted)
{
    SftpXfer *x = container_of(chan, SftpXfer, chan);
    x->input_wanted = wanted;
    if (wanted && x->state == XS_TRANSFER) {
        if (x->job == JOB_SEND)
            xfer_pump_writes(x);
        else
            xfer_pump_reads(x);
    }
}

static char *xfer_log_close_msg(Channel *chan)
{
    return NULL;                       /* (too many of them to log) */
}

/* ----------------------------------------------------------------------
 * Starting a job.
 */

static SftpXfer *xfer_new(Backend *be, XferJob job,
                          const SftpXferClient *client, void *ctx)
{
    ConnectionLayer *cl = be ? ssh_get_connlayer(be) : NULL;
    if (!cl)
        return NULL;
    SftpXfer *x = snew(SftpXfer);
    memset(x, 0, sizeof(*x));
    x->client = client;
    x->ctx = ctx;
    x->cl = cl;
    x->job = job;
    x->state = XS_OPENING;
    x->input_wanted = true;
    bufchain_init(&x->inbuf);
    x->chan.vt = &xfer_channelvt;
    x->chan.initial_fixed_window_size = 0;
    x->sc = ssh_session_open(cl, &x->chan);
    return x;
}

bool sftp_xfer_available(Backend *be)
{
    return be && ssh_get_connlayer(be) != NULL;
}

SftpXfer *sftp_xfer_home(Backend *be, const SftpXferClient *client,
                         void *ctx)
{
    return xfer_new(be, JOB_HOME, client, ctx);
}

SftpXfer *sftp_xfer_list(Backend *be, const SftpXferClient *client,
                         void *ctx, const char *dir)
{
    SftpXfer *x = xfer_new(be, JOB_LIST, client, ctx);
    if (x)
        x->dir = dupstr(dir);
    return x;
}

SftpXfer *sftp_xfer_check(Backend *be, const SftpXferClient *client,
                          void *ctx, const char *path)
{
    SftpXfer *x = xfer_new(be, JOB_CHECK, client, ctx);
    if (x)
        x->path = dupstr(path);
    return x;
}

SftpXfer *sftp_xfer_send(Backend *be, const SftpXferClient *client,
                         void *ctx, const char *dir, const char *name,
                         FILE *fp, uint64_t size, bool overwrite)
{
    SftpXfer *x = xfer_new(be, JOB_SEND, client, ctx);
    if (!x) {
        fclose(fp);
        return NULL;
    }
    x->dir = dupstr(dir);
    x->name = dupstr(name);
    x->fp = fp;
    x->size = size;
    x->overwrite = overwrite;
    return x;
}

SftpXfer *sftp_xfer_receive(Backend *be, const SftpXferClient *client,
                            void *ctx, const char *path, FILE *fp)
{
    SftpXfer *x = xfer_new(be, JOB_RECEIVE, client, ctx);
    if (!x) {
        fclose(fp);
        return NULL;
    }
    x->path = dupstr(path);
    x->fp = fp;
    return x;
}
