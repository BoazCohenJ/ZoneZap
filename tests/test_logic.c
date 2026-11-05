/* Unit tests for src/logic.c. Build and run with `make test`. */
#include "../src/logic.h"

#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                        \
    } while (0)

#define CHECK_STR(got, want)                                                     \
    do {                                                                         \
        checks++;                                                                \
        if (strcmp((got), (want)) != 0) {                                        \
            failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d:\n--- got:\n%s\n--- want:\n%s\n---\n",   \
                    __FILE__, __LINE__, (got), (want));                          \
        }                                                                        \
    } while (0)

static const char ZONE[] = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://example.com/a.zip\r\n";

static void test_match(void)
{
    zz_streams s;
    zz_streams_default(&s);
    size_t bl = 0;
    CHECK(zz_match_name("report.pdf:Zone.Identifier", &s, &bl) == 0);
    CHECK(bl == strlen("report.pdf"));
    CHECK(zz_match_name("report.pdf:zone.identifier", &s, &bl) == 0); /* NTFS is case-insensitive */
    CHECK(zz_match_name("setup.exe:SmartScreen", &s, &bl) == 1);
    CHECK(zz_match_name("a:Zone.Identifier", &s, &bl) == 0 && bl == 1);
    CHECK(zz_match_name("my:file:Zone.Identifier", &s, &bl) == 0 && bl == 7);
    /* not junk */
    CHECK(zz_match_name(":Zone.Identifier", &s, &bl) == -1);          /* no base name */
    CHECK(zz_match_name("Zone.Identifier", &s, &bl) == -1);
    CHECK(zz_match_name("report.pdf", &s, &bl) == -1);
    CHECK(zz_match_name("report.pdf.Zone.Identifier", &s, &bl) == -1);
    CHECK(zz_match_name("report.pdf:Zone.Identifier.bak", &s, &bl) == -1);
    CHECK(zz_match_name("report.pdf:Zone.Identifie", &s, &bl) == -1);
    CHECK(zz_match_name("dir/report.pdf:Zone.Identifier", &s, &bl) == -1);
    CHECK(zz_match_name("x:Other", &s, &bl) == -1);
    CHECK(zz_match_name(NULL, &s, &bl) == -1);
}

static void test_streams_add(void)
{
    zz_streams s;
    zz_streams_default(&s);
    CHECK(s.count == 2);
    CHECK(zz_streams_add(&s, "com.dropbox.attrs") == 0);
    CHECK(zz_streams_add(&s, "ZONE.IDENTIFIER") == -1); /* duplicate */
    CHECK(zz_streams_add(&s, "") == -1);
    CHECK(zz_streams_add(&s, "a/b") == -1);
    CHECK(zz_streams_add(&s, "a:b") == -1);
    CHECK(zz_streams_add(&s, "bad\n") == -1);
    size_t bl;
    CHECK(zz_match_name("x.doc:com.dropbox.attrs", &s, &bl) == 2);
    char big[100];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = 0;
    CHECK(zz_streams_add(&s, big) == -1);
    for (int i = 0; i < 30; i++) {
        char n[16];
        snprintf(n, sizeof n, "s%d", i);
        zz_streams_add(&s, n);
    }
    CHECK(s.count == ZZ_MAX_STREAMS);
}

static void test_judge(void)
{
    zz_streams s;
    zz_streams_default(&s);
    const char *n = "a.zip:Zone.Identifier";
    CHECK(zz_judge(n, 1, sizeof ZONE - 1, ZONE, sizeof ZONE - 1, &s) == ZZ_ZAP);
    CHECK(zz_judge(n, 1, 0, NULL, 0, &s) == ZZ_ZAP); /* empty stream */
    CHECK(zz_judge(n, 0, 10, ZONE, 10, &s) == ZZ_KEEP_NOT_FILE);
    CHECK(zz_judge(n, 1, ZZ_MAX_JUNK_SIZE + 1, ZONE, 20, &s) == ZZ_KEEP_TOO_BIG);
    CHECK(zz_judge(n, 1, -1, ZONE, 20, &s) == ZZ_KEEP_TOO_BIG);
    CHECK(zz_judge(n, 1, 12, "hello world\n", 12, &s) == ZZ_KEEP_CONTENT);
    CHECK(zz_judge(n, 1, 12, NULL, 0, &s) == ZZ_KEEP_CONTENT);
    CHECK(zz_judge("a.zip", 1, 10, ZONE, 10, &s) == ZZ_KEEP_NO_MATCH);
    /* SmartScreen content isn't checked (it's just "Anaheim" from Edge) */
    CHECK(zz_judge("a.exe:SmartScreen", 1, 7, "Anaheim", 7, &s) == ZZ_ZAP);
    CHECK(strcmp(zz_verdict_text(ZZ_ZAP), "removed") == 0);
}

static void test_looks_like_zone(void)
{
    CHECK(zz_looks_like_zone(ZONE, sizeof ZONE - 1));
    CHECK(zz_looks_like_zone("\xEF\xBB\xBF[ZoneTransfer]\r\n", 19));
    CHECK(zz_looks_like_zone("  \r\n[zonetransfer]", 18));
    CHECK(zz_looks_like_zone("", 0));
    CHECK(zz_looks_like_zone("\r\n", 2));
    CHECK(!zz_looks_like_zone("[ZoneTrans", 10));
    CHECK(!zz_looks_like_zone("ZoneId=3", 8));
    /* UTF-16 written by some tools: not recognised, so it is kept (safe side) */
    CHECK(!zz_looks_like_zone("\xFF\xFE[\0Z\0", 6));
}

static void test_parse_zone(void)
{
    zz_zone_info z;
    zz_parse_zone(ZONE, sizeof ZONE - 1, &z);
    CHECK(z.zone_id == 3);
    CHECK_STR(z.host_url, "https://example.com/a.zip");
    CHECK_STR(z.referrer_url, "");
    CHECK_STR(zz_zone_name(z.zone_id), "Internet");

    const char *b = "[ZoneTransfer]\nReferrerUrl = https://ex.org/page \nzoneid = 2\nHostUrl=about:internet";
    zz_parse_zone(b, strlen(b), &z);
    CHECK(z.zone_id == 2);
    CHECK_STR(z.referrer_url, "https://ex.org/page");
    CHECK_STR(z.host_url, "about:internet");

    const char *bad = "[ZoneTransfer]\nZoneId=9\nZoneId\nHostUrl";
    zz_parse_zone(bad, strlen(bad), &z);
    CHECK(z.zone_id == -1);
    CHECK_STR(z.host_url, "");
    CHECK_STR(zz_zone_name(-1), "Unknown");
    CHECK_STR(zz_zone_name(0), "Local machine");

    zz_parse_zone("ZoneId=3x", 9, &z);
    CHECK(z.zone_id == -1);

    /* very long URL is cut, not overflowed */
    char longb[2000] = "HostUrl=";
    memset(longb + 8, 'u', 1500);
    longb[1508] = 0;
    zz_parse_zone(longb, strlen(longb), &z);
    CHECK(strlen(z.host_url) == sizeof z.host_url - 1);
}

static void test_zone_should_replace(void)
{
    const char *full = ZONE;
    const char *bare = "[ZoneTransfer]\r\nZoneId=3\r\n";
    const char *other = "[ZoneTransfer]\r\nZoneId=3\r\nHostUrl=https://other.example/b.zip\r\n";
    const char *ref = "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://ex.org/\r\n";
    /* Explorer's bare re-mark must not wipe the URL */
    CHECK(!zz_zone_should_replace(full, strlen(full), bare, strlen(bare)));
    CHECK(!zz_zone_should_replace(ref, strlen(ref), bare, strlen(bare)));
    /* anything with a URL replaces */
    CHECK(zz_zone_should_replace(full, strlen(full), other, strlen(other)));
    CHECK(zz_zone_should_replace(bare, strlen(bare), full, strlen(full)));
    /* bare over bare, or over garbage: replace */
    CHECK(zz_zone_should_replace(bare, strlen(bare), bare, strlen(bare)));
    CHECK(zz_zone_should_replace("junk", 4, bare, strlen(bare)));
}

static void test_xattr_name(void)
{
    char n[64];
    CHECK(zz_xattr_name("Zone.Identifier", n, sizeof n) == 0);
    CHECK_STR(n, "user.windows.Zone.Identifier");
    CHECK(zz_xattr_name("Zone.Identifier", n, 10) == -1);
}

#define CMD "/usr/local/bin/zonezap run --daemon"

static void wsl_add(const char *in, const char *want)
{
    char out[4096];
    int n = zz_wslconf_add(in, CMD, out, sizeof out);
    CHECK(n >= 0);
    CHECK_STR(out, want);
    /* adding twice changes nothing */
    char again[4096];
    zz_wslconf_add(out, CMD, again, sizeof again);
    CHECK_STR(again, want);
}

static void test_wslconf(void)
{
    wsl_add("", "[boot]\ncommand = " CMD "\n");
    wsl_add("[automount]\nenabled = true\n",
            "[automount]\nenabled = true\n\n[boot]\ncommand = " CMD "\n");
    wsl_add("[automount]\nenabled = true", /* no final newline */
            "[automount]\nenabled = true\n\n[boot]\ncommand = " CMD "\n");
    wsl_add("[boot]\nsystemd=true\n", "[boot]\nsystemd=true\ncommand = " CMD "\n");
    wsl_add("[boot]\nsystemd=false\n[user]\ndefault=me\n",
            "[boot]\nsystemd=false\ncommand = " CMD "\n[user]\ndefault=me\n");
    wsl_add("[boot]\ncommand = service docker start\n",
            "[boot]\ncommand = " CMD "; service docker start\n");
    wsl_add("[boot]\ncommand=\"service docker start\"\n",
            "[boot]\ncommand=\"" CMD "; service docker start\"\n");
    wsl_add("[Boot]\r\n  Command = mount -a; /opt/zonezap run --daemon\r\n",
            "[Boot]\r\n  Command = " CMD "; mount -a\n");
    /* a command= line in another section is not ours to touch */
    wsl_add("[other]\ncommand = x\n", "[other]\ncommand = x\n\n[boot]\ncommand = " CMD "\n");

    char out[4096];
    zz_wslconf_remove("[boot]\ncommand = " CMD "; service docker start\n", out, sizeof out);
    CHECK_STR(out, "[boot]\ncommand = service docker start\n");
    zz_wslconf_remove("[boot]\ncommand = " CMD "\nsystemd=false\n", out, sizeof out);
    CHECK_STR(out, "[boot]\nsystemd=false\n");
    zz_wslconf_remove("[boot]\ncommand=\"" CMD "\"\n", out, sizeof out);
    CHECK_STR(out, "[boot]\n");
    zz_wslconf_remove("[network]\nhostname=x\n", out, sizeof out);
    CHECK_STR(out, "[network]\nhostname=x\n");

    char tiny[8];
    CHECK(zz_wslconf_add("[boot]\n", CMD, tiny, sizeof tiny) == -1);
}

static void test_fs(void)
{
    CHECK(zz_fs_type_watchable("ext4"));
    CHECK(zz_fs_type_watchable("btrfs"));
    CHECK(!zz_fs_type_watchable("9p"));
    CHECK(!zz_fs_type_watchable("drvfs"));
    CHECK(!zz_fs_type_watchable("virtiofs"));
    CHECK(!zz_fs_type_watchable("tmpfs"));
    CHECK(!zz_fs_type_watchable("overlay"));
    CHECK(zz_mount_point_skipped("/mnt/wsl"));
    CHECK(zz_mount_point_skipped("/mnt/wslg/distro"));
    CHECK(zz_mount_point_skipped("/usr/lib/wsl/drivers"));
    CHECK(!zz_mount_point_skipped("/"));
    CHECK(!zz_mount_point_skipped("/mnt/wsldata"));
    CHECK(!zz_mount_point_skipped("/home"));
}

int main(void)
{
    test_match();
    test_streams_add();
    test_judge();
    test_looks_like_zone();
    test_parse_zone();
    test_zone_should_replace();
    test_xattr_name();
    test_wslconf();
    test_fs();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
