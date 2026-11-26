/* session.c - the session folder: session.json, events.jsonl, frames/, files/
 * (format: docs/FORMAT.md).  events.jsonl is appended line by line and
 * flushed at least once a second, so a crash or a pulled USB stick leaves a
 * readable session. */
#include "core.h"

volatile long g_frames_stored, g_mark_n, g_sync_n;

#define MAXSRC 32
static struct { char id[16]; char *json; } srcs[MAXSRC];
static int nsrc;

static volatile int active;
static void *hev;
static char root[TICK_PATH], dir[TICK_PATH];
static u64 started_us, ended_us, bytes;
static u32 last_flush;
static int write_failed, dirty;
static char note[256];
static char capture_proc[TICK_PATH];

static void lock(void) { p_lock(); }
static void unlock(void) { p_unlock(); }

void copy_str(char *d, const char *s, u32 cap)
{
    u32 i = 0;
    if (!cap) return;
    while (s && s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void cat_str(char *d, const char *s, u32 cap)
{
    u32 n = (u32)strlen(d);
    if (n < cap) copy_str(d + n, s, cap - n);
}

int session_active(void) { return active; }
const char *session_dir(void) { return dir; }
const char *session_root(void) { return root; }
u32 session_bytes_kb(void) { return (u32)(bytes >> 10); }


static void write_session_json(void)
{
    sbuf b;
    char host[64];
    char path[TICK_PATH];
    void *h;
    int i;

    sb_init(&b, 2048);
    sb_str(&b, "{\n  \"format\": \"libretap-session\",\n  \"version\": 1,\n  \"recorder\": \"" TICK_NAME "\",\n  \"host\": ");
    p_hostname(host, sizeof host);
    {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, host);
        sb_json(&b, u.p, u.len);
        sb_free(&u);
    }
    sb_str(&b, ",\n  \"os\": \"");
    p_os(&b);
    sb_str(&b, "\",\n  \"started\": \"");
    clock_iso(&b, started_us);
    sb_str(&b, "\",\n");
    if (ended_us) {
        sb_str(&b, "  \"ended\": \"");
        clock_iso(&b, ended_us);
        sb_str(&b, "\",\n");
    }
    sb_str(&b, "  \"clock\": \"Recorder's own wall clock; event t = seconds since 1970-01-01 UTC by that clock\",\n"
               "  \"utc_offset_min\": ");
    sb_i32(&b, clock_utc_offset_min());
    sb_str(&b, ",\n  \"sources\": [");
    for (i = 0; i < nsrc; i++) {
        sb_str(&b, i ? ",\n    " : "\n    ");
        sb_str(&b, srcs[i].json);
    }
    sb_str(&b, "\n  ],\n  \"tick\": {\"interval_ms\": ");
    sb_u32(&b, g_cfg.interval_ms);
    sb_str(&b, ", \"full_every\": ");
    sb_u32(&b, g_cfg.full_every);
    sb_str(&b, ", \"tile\": ");
    sb_u32(&b, g_cfg.tile);
    sb_str(&b, g_cfg.cursor ? ", \"cursor\": true" : ", \"cursor\": false");
    sb_str(&b, g_cfg.mask_self ? ", \"tick_window_masked\": true" : ", \"tick_window_masked\": false");
    sb_str(&b, g_cfg.program_only ? ", \"capture\": \"program-windows\"" : ", \"capture\": \"screen\"");
    if (g_cfg.program_only) {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, capture_proc);
        sb_str(&b, ", \"capture_process\": ");
        sb_json(&b, u.p ? u.p : "", u.len);
        sb_free(&u);
    }
    sb_putc(&b, '}');
    sb_str(&b, ",\n  \"note\": ");
    {
        sbuf u;
        sb_init(&u, 64);
        ansi_to_utf8(&u, note);
        sb_json(&b, u.p, u.len);
        sb_free(&u);
    }
    sb_str(&b, "\n}\n");

    copy_str(path, dir, TICK_PATH);
    cat_str(path, "session.json", TICK_PATH);
    h = pf_create(path, 0);
    if (h) {
        if (!pf_write(h, b.p, b.len)) write_failed = 1;
        pf_flush(h);
        pf_close(h);
    } else write_failed = 1;
    sb_free(&b);
}


/* try to create <base>\<name> with an events.jsonl in it */
static int try_root(const char *base)
{
    tick_tm st;
    char host[64], name[80], path[TICK_PATH];
    sbuf b;
    int k, i, r;

    if (!base[0]) return 0;
    copy_str(root, base, TICK_PATH);
    if (root[strlen(root) - 1] != PSEP[0]) cat_str(root, PSEP, TICK_PATH);
    if (!p_mkdir(root)) return 0;

    clock_tm(started_us, &st);
    p_hostname(host, sizeof host);
    sb_init(&b, 64);
    sb_u32w(&b, (u32)st.year, 4); sb_u32w(&b, (u32)st.mon, 2); sb_u32w(&b, (u32)st.day, 2);
    sb_putc(&b, '-');
    sb_u32w(&b, (u32)st.hour, 2); sb_u32w(&b, (u32)st.min, 2); sb_u32w(&b, (u32)st.sec, 2);
    if (host[0]) {
        sb_putc(&b, '-');
        for (i = 0; host[i] && i < 24; i++) {
            char c = host[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) c = '_';
            sb_putc(&b, c);
        }
    }
    copy_str(name, b.p, sizeof name);
    sb_free(&b);
    for (k = 1; k < 20; k++) {
        copy_str(dir, root, TICK_PATH);
        cat_str(dir, name, TICK_PATH);
        if (k > 1) {
            char s[4];
            s[0] = '-'; s[1] = (char)('0' + k / 10); s[2] = (char)('0' + k % 10); s[3] = 0;
            if (k < 10) { s[1] = (char)('0' + k); s[2] = 0; }
            cat_str(dir, s, TICK_PATH);
        }
        r = p_mkdir_new(dir);
        if (r == 1) break;
        if (r < 0) return 0;
    }
    if (k == 20) return 0;
    cat_str(dir, PSEP, TICK_PATH);
    copy_str(path, dir, TICK_PATH);
    cat_str(path, "events.jsonl", TICK_PATH);
    hev = pf_create(path, 1);
    if (!hev) {
        p_rmdir(dir);
        return 0;
    }
    return 1;
}

int session_start(char *err, u32 errcap)
{
    char base[TICK_PATH];
    int ok = 0;
    sbuf b;

    if (active) return 1;
    lock();
    started_us = clock_now_us();
    ended_us = 0;
    capture_proc[0] = 0;
    bytes = 0;
    write_failed = 0;
    nsrc = 0;
    g_frames_stored = 0;
    g_mark_n = 0;
    g_sync_n = 0;
    if (g_cfg.dir[0]) ok = try_root(g_cfg.dir);
    if (!ok) {
        copy_str(base, g_exe_dir, TICK_PATH);
        cat_str(base, "TickSessions", TICK_PATH);
        ok = try_root(base);
    }
    if (!ok && p_documents(base, TICK_PATH)) {
        cat_str(base, PSEP "TickSessions", TICK_PATH);
        ok = try_root(base);
    }
    if (!ok && p_temp(base, TICK_PATH)) {
        if (base[0] && base[strlen(base) - 1] != PSEP[0]) cat_str(base, PSEP, TICK_PATH);
        cat_str(base, "TickSessions", TICK_PATH);
        ok = try_root(base);
    }
    if (!ok) {
        unlock();
        copy_str(err, "Could not create a session folder (next to Tick, in Documents/home or in the temp folder).", errcap);
        return 0;
    }
    session_mkdir("frames");
    active = 1;
    write_session_json();
    last_flush = p_ms();
    unlock();

    sb_init(&b, 256);
    sb_str(&b, "Tick started recording into ");
    ansi_to_utf8(&b, dir);
    ev_status("status", b.p);
    sb_free(&b);
    return 1;
}

void session_stop(void)
{
    if (!active) return;
    ev_status("status", "Tick stopped recording");
    lock();
    active = 0;
    ended_us = clock_now_us();
    pf_flush(hev);
    pf_close(hev);
    hev = 0;
    write_session_json();
    {
        int i;
        for (i = 0; i < nsrc; i++) { rt_free(srcs[i].json); srcs[i].json = 0; }
        nsrc = 0;
    }
    unlock();
}

void session_set_note(const char *n)
{
    lock();
    copy_str(note, n, sizeof note);
    if (active) write_session_json();
    unlock();
}

void ev_begin(sbuf *b, u64 t, const char *kind)
{
    sb_reset(b);
    sb_str(b, "{\"t\":");
    sb_time(b, t);
    sb_str(b, ",\"kind\":\"");
    sb_str(b, kind);
    sb_putc(b, '"');
}

/* is this the line of an event of the given kind? (ev_begin's layout) */
static int has_kind(const sbuf *b, const char *kind)
{
    u32 i, n = (u32)strlen(kind);
    for (i = 0; i + 9 + n < b->len && i < 40; i++)
        if (memcmp(b->p + i, ",\"kind\":\"", 9) == 0)
            return memcmp(b->p + i + 9, kind, n) == 0 && b->p[i + 9 + n] == '"';
    return 0;
}

void ev_end(sbuf *b)
{
    int urgent;
    sb_str(b, "}\n");
    if (b->oom) return;
    urgent = has_kind(b, "mark") || has_kind(b, "sync") || has_kind(b, "error");
    lock();
    if (active && hev) {
        u32 now = p_ms();
        if (!pf_write(hev, b->p, b->len)) write_failed = 1;
        else { bytes += b->len; dirty = 1; }
        /* marks and syncs are flushed at once, everything else at most 1 s later */
        if (urgent || now - last_flush >= 1000) {
            pf_flush(hev);
            last_flush = now;
            dirty = 0;
        }
    }
    unlock();
}

void ev_status(const char *kind, const char *text)
{
    sbuf b;
    sb_init(&b, 128);
    ev_begin(&b, clock_now_us(), kind);
    sb_str(&b, ",\"text\":");
    sb_jsonz(&b, text);
    ev_end(&b);
    sb_free(&b);
}

int session_has_source(const char *id)
{
    int i, r = 0;
    lock();
    for (i = 0; i < nsrc; i++) if (strcmp(srcs[i].id, id) == 0) r = 1;
    unlock();
    return r;
}

void session_add_source(const char *id, const char *json)
{
    int i;
    lock();
    for (i = 0; i < nsrc; i++) if (strcmp(srcs[i].id, id) == 0) break;
    if (i == nsrc && nsrc < MAXSRC && active) {
        u32 n = (u32)strlen(json);
        srcs[nsrc].json = rt_alloc(n + 1);
        if (srcs[nsrc].json) {
            memcpy(srcs[nsrc].json, json, n);
            copy_str(srcs[nsrc].id, id, sizeof srcs[nsrc].id);
            nsrc++;
            write_session_json();
        }
    }
    unlock();
}

void session_mkdir(const char *rel)
{
    char path[TICK_PATH];
    copy_str(path, dir, TICK_PATH);
    cat_str(path, rel, TICK_PATH);
    p_mkdir(path);
}

int session_write_file(const char *rel, const void *p, u32 n)
{
    char path[TICK_PATH];
    void *h;
    int ok;
    copy_str(path, dir, TICK_PATH);
    cat_str(path, rel, TICK_PATH);
    h = pf_create(path, 0);
    if (!h) return 0;
    ok = pf_write(h, p, n);
    pf_close(h);
    if (!ok) { p_unlink(path); return 0; }
    lock();
    bytes += n;
    unlock();
    return 1;
}

/* called about once a second by the window: flush a quiet events.jsonl */
void session_flush_if_due(void)
{
    lock();
    if (active && dirty && p_ms() - last_flush >= 1000) {
        pf_flush(hev);
        last_flush = p_ms();
        dirty = 0;
    }
    unlock();
}

int session_write_failed(void) { return write_failed; }

void session_set_capture(const char *process)
{
    lock();
    copy_str(capture_proc, process, sizeof capture_proc);
    if (active) write_session_json();
    unlock();
}
