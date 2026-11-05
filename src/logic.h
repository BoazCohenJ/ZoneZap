/* ZoneZap decision logic. Pure functions, no I/O, so they can be unit tested. */
#ifndef ZONEZAP_LOGIC_H
#define ZONEZAP_LOGIC_H

#include <stddef.h>

#define ZZ_MAX_STREAMS 16
#define ZZ_MAX_STREAM_NAME 64
/* Real Zone.Identifier files are a few hundred bytes. Anything bigger is left alone. */
#define ZZ_MAX_JUNK_SIZE 65536

typedef struct {
    int count;
    char names[ZZ_MAX_STREAMS][ZZ_MAX_STREAM_NAME];
} zz_streams;

/* Fills in the default stream list: Zone.Identifier and SmartScreen. */
void zz_streams_default(zz_streams *s);
/* Adds a stream name. Returns 0 on success, -1 if invalid, full or a duplicate. */
int zz_streams_add(zz_streams *s, const char *name);

/* If `name` looks like "<base>:<stream>" for a stream in the list (stream compared
 * case-insensitively, like NTFS), returns the index of the stream and stores the
 * length of <base> in *base_len. Returns -1 otherwise. <base> must be non-empty. */
int zz_match_name(const char *name, const zz_streams *s, size_t *base_len);

typedef enum {
    ZZ_ZAP = 0,          /* delete it */
    ZZ_KEEP_NO_MATCH,    /* name is not <base>:<known stream> */
    ZZ_KEEP_NOT_FILE,    /* directory, symlink, device... */
    ZZ_KEEP_TOO_BIG,     /* bigger than ZZ_MAX_JUNK_SIZE */
    ZZ_KEEP_CONTENT      /* Zone.Identifier whose content isn't a [ZoneTransfer] block */
} zz_verdict;

/* Decides whether a directory entry is Windows stream junk.
 * is_regular: 1 for a regular file (not following symlinks).
 * head/head_len: the first bytes of the file (may be NULL if size is 0). */
zz_verdict zz_judge(const char *name, int is_regular, long long size,
                    const char *head, size_t head_len, const zz_streams *s);
const char *zz_verdict_text(zz_verdict v);

/* True if a Zone.Identifier body starts with "[ZoneTransfer]" (optionally after a
 * UTF-8 BOM or whitespace), or is empty. */
int zz_looks_like_zone(const char *head, size_t len);

typedef struct {
    int zone_id;             /* -1 if missing */
    char host_url[512];
    char referrer_url[512];
} zz_zone_info;

/* Parses ZoneId / HostUrl / ReferrerUrl out of a Zone.Identifier body. */
void zz_parse_zone(const char *buf, size_t len, zz_zone_info *out);
/* "Local machine", "Intranet", "Trusted", "Internet", "Restricted" or "Unknown". */
const char *zz_zone_name(int zone_id);

/* Name of the extended attribute that keeps a stream's content: user.windows.<stream>. */
int zz_xattr_name(const char *stream, char *out, size_t out_size);

/* Edits /etc/wsl.conf text so that [boot] command also starts ZoneZap.
 * `start_cmd` is the shell command to add. An existing command is kept and ours is
 * prepended with "; ". If ZoneZap is already present it is replaced. Returns the
 * length written, or -1 if `out` is too small. */
int zz_wslconf_add(const char *in, const char *start_cmd, char *out, size_t out_size);
/* Removes ZoneZap from the [boot] command again (and the line, if nothing is left). */
int zz_wslconf_remove(const char *in, char *out, size_t out_size);

/* Filesystems ZoneZap watches by default: real local disks, not 9p/drvfs/virtiofs. */
int zz_fs_type_watchable(const char *fstype);
/* Mount points that are WSL plumbing and never hold user files. */
int zz_mount_point_skipped(const char *mount_point);

#endif
