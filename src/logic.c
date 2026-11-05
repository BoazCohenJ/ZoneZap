#include "logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zz_streams_default(zz_streams *s)
{
    s->count = 0;
    zz_streams_add(s, "Zone.Identifier");
    zz_streams_add(s, "SmartScreen");
}

int zz_streams_add(zz_streams *s, const char *name)
{
    size_t n = strlen(name);
    if (n == 0 || n >= ZZ_MAX_STREAM_NAME || s->count >= ZZ_MAX_STREAMS)
        return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == ':' || c < 0x20)
            return -1;
    }
    for (int i = 0; i < s->count; i++)
        if (strcasecmp(s->names[i], name) == 0)
            return -1;
    memcpy(s->names[s->count], name, n + 1);
    s->count++;
    return 0;
}

int zz_match_name(const char *name, const zz_streams *s, size_t *base_len)
{
    if (!name || strchr(name, '/'))
        return -1;
    size_t len = strlen(name);
    for (int i = 0; i < s->count; i++) {
        size_t sl = strlen(s->names[i]);
        /* need at least one character of base, then ':', then the stream */
        if (len < sl + 2)
            continue;
        const char *colon = name + len - sl - 1;
        if (*colon != ':')
            continue;
        if (strcasecmp(colon + 1, s->names[i]) != 0)
            continue;
        if (base_len)
            *base_len = (size_t)(colon - name);
        return i;
    }
    return -1;
}

int zz_looks_like_zone(const char *head, size_t len)
{
    size_t i = 0;
    if (len >= 3 && (unsigned char)head[0] == 0xEF && (unsigned char)head[1] == 0xBB &&
        (unsigned char)head[2] == 0xBF)
        i = 3;
    while (i < len && isspace((unsigned char)head[i]))
        i++;
    if (i == len)
        return 1; /* empty (or only whitespace) */
    static const char tag[] = "[ZoneTransfer]";
    size_t tl = sizeof(tag) - 1;
    if (len - i < tl)
        return 0;
    return strncasecmp(head + i, tag, tl) == 0;
}

zz_verdict zz_judge(const char *name, int is_regular, long long size,
                    const char *head, size_t head_len, const zz_streams *s)
{
    size_t base_len;
    int idx = zz_match_name(name, s, &base_len);
    if (idx < 0)
        return ZZ_KEEP_NO_MATCH;
    if (!is_regular)
        return ZZ_KEEP_NOT_FILE;
    if (size < 0 || size > ZZ_MAX_JUNK_SIZE)
        return ZZ_KEEP_TOO_BIG;
    if (strcasecmp(s->names[idx], "Zone.Identifier") == 0 && size > 0) {
        if (!head || !zz_looks_like_zone(head, head_len))
            return ZZ_KEEP_CONTENT;
    }
    return ZZ_ZAP;
}

const char *zz_verdict_text(zz_verdict v)
{
    switch (v) {
    case ZZ_ZAP: return "removed";
    case ZZ_KEEP_NO_MATCH: return "not a stream file";
    case ZZ_KEEP_NOT_FILE: return "kept: not a regular file";
    case ZZ_KEEP_TOO_BIG: return "kept: too big to be a Windows stream";
    case ZZ_KEEP_CONTENT: return "kept: content is not a [ZoneTransfer] block";
    }
    return "?";
}

static void copy_value(char *dst, size_t dst_size, const char *src, size_t n)
{
    if (n >= dst_size)
        n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void zz_parse_zone(const char *buf, size_t len, zz_zone_info *out)
{
    out->zone_id = -1;
    out->host_url[0] = 0;
    out->referrer_url[0] = 0;
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && buf[i] != '\n' && buf[i] != '\r')
            i++;
        size_t end = i;
        while (i < len && (buf[i] == '\n' || buf[i] == '\r'))
            i++;
        const char *line = buf + start;
        size_t ll = end - start;
        const char *eq = memchr(line, '=', ll);
        if (!eq)
            continue;
        size_t kl = (size_t)(eq - line);
        while (kl > 0 && isspace((unsigned char)line[kl - 1]))
            kl--;
        const char *v = eq + 1;
        size_t vl = ll - (size_t)(v - line);
        while (vl > 0 && isspace((unsigned char)*v)) {
            v++;
            vl--;
        }
        while (vl > 0 && isspace((unsigned char)v[vl - 1]))
            vl--;
        if (kl == 6 && strncasecmp(line, "ZoneId", 6) == 0) {
            char tmp[16];
            copy_value(tmp, sizeof tmp, v, vl);
            char *endp;
            long z = strtol(tmp, &endp, 10);
            if (endp != tmp && *endp == 0 && z >= 0 && z <= 4)
                out->zone_id = (int)z;
        } else if (kl == 7 && strncasecmp(line, "HostUrl", 7) == 0) {
            copy_value(out->host_url, sizeof out->host_url, v, vl);
        } else if (kl == 11 && strncasecmp(line, "ReferrerUrl", 11) == 0) {
            copy_value(out->referrer_url, sizeof out->referrer_url, v, vl);
        }
    }
}

const char *zz_zone_name(int zone_id)
{
    switch (zone_id) {
    case 0: return "Local machine";
    case 1: return "Local intranet";
    case 2: return "Trusted sites";
    case 3: return "Internet";
    case 4: return "Restricted sites";
    }
    return "Unknown";
}

int zz_zone_should_replace(const char *old_buf, size_t old_len, const char *new_buf, size_t new_len)
{
    zz_zone_info o, n;
    zz_parse_zone(old_buf, old_len, &o);
    zz_parse_zone(new_buf, new_len, &n);
    int old_has_url = o.host_url[0] || o.referrer_url[0];
    int new_has_url = n.host_url[0] || n.referrer_url[0];
    return !(old_has_url && !new_has_url);
}

int zz_xattr_name(const char *stream, char *out, size_t out_size)
{
    int n = snprintf(out, out_size, "user.windows.%s", stream);
    return (n < 0 || (size_t)n >= out_size) ? -1 : 0;
}

/* ---------- wsl.conf editing ---------- */

typedef struct {
    char *buf;
    size_t size;
    size_t len;
    int overflow;
} sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->overflow || b->len + n + 1 > b->size) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

static const char *rtrim(const char *start, const char *end)
{
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return end;
}

/* Is this line a section header? If so, set *is_boot. */
static int is_section(const char *l, const char *e, int *is_boot)
{
    l = skip_ws(l, e);
    e = rtrim(l, e);
    if (e - l < 2 || *l != '[' || e[-1] != ']')
        return 0;
    const char *n = skip_ws(l + 1, e - 1);
    const char *ne = rtrim(n, e - 1);
    *is_boot = (ne - n == 4 && strncasecmp(n, "boot", 4) == 0);
    return 1;
}

/* Is this line "command = value"? If so return pointers to the raw value. */
static int is_command(const char *l, const char *e, const char **v, const char **ve)
{
    l = skip_ws(l, e);
    if (e - l < 7 || strncasecmp(l, "command", 7) != 0)
        return 0;
    const char *p = skip_ws(l + 7, e);
    if (p >= e || *p != '=')
        return 0;
    p = skip_ws(p + 1, e);
    *v = p;
    *ve = rtrim(p, e);
    return 1;
}

static int contains_ci(const char *s, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    for (size_t i = 0; i + nl <= n; i++)
        if (strncasecmp(s + i, needle, nl) == 0)
            return 1;
    return 0;
}

/* Writes the command value without ZoneZap's own segments. Returns 1 if anything is left. */
static int put_others(sbuf *b, const char *v, const char *ve)
{
    int wrote = 0;
    const char *p = v;
    while (p < ve) {
        const char *q = p;
        while (q < ve && *q != ';')
            q++;
        const char *s = skip_ws(p, q);
        const char *se = rtrim(s, q);
        if (se > s && !contains_ci(s, (size_t)(se - s), "zonezap")) {
            if (wrote)
                sb_puts(b, "; ");
            sb_put(b, s, (size_t)(se - s));
            wrote = 1;
        }
        p = q < ve ? q + 1 : q;
    }
    return wrote;
}

static int others_left(const char *v, const char *ve)
{
    char tmp[4096];
    sbuf t = {tmp, sizeof tmp, 0, 0};
    tmp[0] = 0;
    return put_others(&t, v, ve);
}

static int edit(const char *in, const char *start_cmd, char *out, size_t out_size)
{
    sbuf b = {out, out_size, 0, 0};
    if (out_size)
        out[0] = 0;
    int in_boot = 0, done = 0, saw_boot = 0;
    const char *p = in;
    const char *end = in + strlen(in);
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        const char *next = e ? e + 1 : end;
        if (!e)
            e = end;
        const char *le = (e > p && e[-1] == '\r') ? e - 1 : e;
        int boot;
        const char *v, *ve;
        if (is_section(p, le, &boot)) {
            if (in_boot && !done && start_cmd) {
                /* [boot] had no command line: add ours at the end of the section */
                sb_puts(&b, "command = ");
                sb_puts(&b, start_cmd);
                sb_puts(&b, "\n");
                done = 1;
            }
            in_boot = boot;
            if (boot)
                saw_boot = 1;
            sb_put(&b, p, (size_t)(next - p));
        } else if (in_boot && !done && is_command(p, le, &v, &ve)) {
            int quoted = (ve - v >= 2 && *v == '"' && ve[-1] == '"');
            const char *iv = quoted ? v + 1 : v;
            const char *ive = quoted ? ve - 1 : ve;
            int left = others_left(iv, ive);
            if (start_cmd || left) {
                sb_put(&b, p, (size_t)(v - p));
                if (quoted)
                    sb_puts(&b, "\"");
                if (start_cmd) {
                    sb_puts(&b, start_cmd);
                    if (left)
                        sb_puts(&b, "; ");
                }
                put_others(&b, iv, ive);
                if (quoted)
                    sb_puts(&b, "\"");
                sb_puts(&b, "\n");
            }
            done = 1;
        } else {
            sb_put(&b, p, (size_t)(next - p));
        }
        p = next;
    }
    if (start_cmd && !done) {
        if (b.len > 0 && out[b.len - 1] != '\n')
            sb_puts(&b, "\n"); /* last line had no newline */
        if (in_boot) {
            sb_puts(&b, "command = ");
        } else {
            if (b.len > 0)
                sb_puts(&b, "\n");
            sb_puts(&b, "[boot]\ncommand = ");
        }
        (void)saw_boot;
        sb_puts(&b, start_cmd);
        sb_puts(&b, "\n");
    }
    return b.overflow ? -1 : (int)b.len;
}

int zz_wslconf_add(const char *in, const char *start_cmd, char *out, size_t out_size)
{
    return edit(in, start_cmd, out, out_size);
}

int zz_wslconf_remove(const char *in, char *out, size_t out_size)
{
    return edit(in, NULL, out, out_size);
}

/* ---------- filesystems ---------- */

int zz_fs_type_watchable(const char *t)
{
    static const char *ok[] = {"ext4", "ext3", "ext2", "xfs", "btrfs", "f2fs", NULL};
    for (int i = 0; ok[i]; i++)
        if (strcmp(t, ok[i]) == 0)
            return 1;
    return 0;
}

int zz_mount_point_skipped(const char *m)
{
    static const char *skip[] = {"/mnt/wsl", "/mnt/wslg", "/usr/lib/wsl", "/init", NULL};
    for (int i = 0; skip[i]; i++) {
        size_t n = strlen(skip[i]);
        if (strncmp(m, skip[i], n) == 0 && (m[n] == 0 || m[n] == '/'))
            return 1;
    }
    return 0;
}
