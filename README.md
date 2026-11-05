# ZoneZap

**Stops `file:Zone.Identifier` junk from piling up in WSL.** When Windows copies a downloaded file into a WSL distro (Explorer, a browser saving to `\\wsl.localhost\…`, `robocopy`, `Copy-Item`, VS Code drag and drop), it writes the file's hidden "downloaded from the internet" stream as a second, visible file next to it:

```
report.pdf
report.pdf:Zone.Identifier     <- this one
```

These end up in `git status`, Docker build contexts and backups. Microsoft has had this open since 2019 ([microsoft/WSL#4609](https://github.com/microsoft/WSL/issues/4609), [microsoft/WSL#7456](https://github.com/microsoft/WSL/issues/7456), 570+ 👍 between them). The registry and policy tweaks people pass around only cover new downloads, and they turn off a Windows security feature for everything else too.

ZoneZap is a ~100 KB static Linux daemon that runs inside the distro. It watches whole filesystems with fanotify and deletes each `:Zone.Identifier` file as soon as Windows has finished writing it: about 1 ms later in my tests. The "where did this come from" information isn't thrown away; it's kept as an extended attribute on the real file. Windows itself is not changed.

> **Honest scope:** ZoneZap can't stop Windows from creating these files, only remove them the moment they appear. A program that lists the folder in that millisecond could still see one.

**Website:** https://boazcohenj.github.io/ZoneZap/

## Install

Inside your WSL distro:

```sh
curl -fsSL https://boazcohenj.github.io/ZoneZap/install.sh | sudo sh
```

The installer downloads the binary for your CPU (x86_64 or arm64) from the [latest release](https://github.com/BoazCohenJ/ZoneZap/releases/latest), checks its SHA-256, copies it to `/usr/local/bin/zonezap` and starts it with the distro:

- with systemd on (the default for Ubuntu on WSL): as `zonezap.service`;
- without systemd (Alpine, older setups): through the `[boot]` command in `/etc/wsl.conf`. An existing boot command is kept; ZoneZap is added in front of it.

Then clean up what's already there and check it works:

```sh
sudo zonezap clean        # remove existing leftovers (add --dry-run to just list them)
sudo zonezap self-test    # copies a file from Windows into the distro and checks it's cleaned up
zonezap status
```

Each distro has its own filesystem, so install ZoneZap in every distro you copy files into.

Prefer not to pipe into `sh`? Download `zonezap-linux-x86_64` (or `-aarch64`) and its `.sha256` from the release, run `sha256sum -c zonezap-linux-x86_64.sha256`, then `chmod +x zonezap-linux-x86_64 && sudo ./zonezap-linux-x86_64 install`.

**Uninstall:** `sudo zonezap uninstall` removes the service or the `wsl.conf` entry and the binary. Your `wsl.conf` is backed up to `/etc/wsl.conf.zonezap-bak` the first time it's edited.

## What it removes, and what it never touches

ZoneZap only deletes a file when **all** of these hold:

- its name is `<something>:Zone.Identifier` or `<something>:SmartScreen` (the stream Edge adds), compared without case like NTFS does;
- it is a regular file: never a directory, symlink or device;
- it is at most 64 KB;
- for `:Zone.Identifier`, its content is empty or starts with `[ZoneTransfer]`, the format Windows writes. A file of your own that happens to have that name is kept.

It also only watches local Linux disks (ext4, xfs, btrfs…), never `/mnt/c` or other Windows drives, where these names can't exist anyway.

Other Windows streams can be added with `--stream NAME`, for example `sudo zonezap install --stream com.dropbox.attrs`.

## Where the zone info goes

Before deleting `report.pdf:Zone.Identifier`, ZoneZap stores its content in the `user.windows.Zone.Identifier` extended attribute of `report.pdf`. So you can still see where a file came from:

```
$ zonezap info report.pdf
report.pdf
 Zone.Identifier (kept by ZoneZap):
  Zone:      3 (Internet)
  Came from: https://example.com/report.pdf
  Page:      https://example.com/downloads
```

(`getfattr -n user.windows.Zone.Identifier report.pdf` shows the raw text.) Turn this off with `--no-xattr`. The attribute isn't copied back to Windows, so a file copied from WSL to Windows won't carry a "downloaded from the internet" mark. That's the same as without ZoneZap: Windows ignores these Linux-side files.

## Commands

```
sudo zonezap install       Copy to /usr/local/bin and start with this distro
sudo zonezap uninstall     Undo install
zonezap status             Is it running, and what has it removed
zonezap clean [PATH...]    Remove existing leftovers (default: / as root, else your home)
zonezap info FILE...       Show where a file came from (kept zone info)
sudo zonezap self-test     Copy a file from Windows and check it works
sudo zonezap run           Watch in the foreground (what install starts)

--stream NAME   Also remove <file>:NAME (default: Zone.Identifier, SmartScreen)
--no-xattr      Don't keep the content in user.windows.<stream> xattrs
--dry-run       With clean: only list what would be removed
--daemon        With run: go to the background, log to /var/log/zonezap.log
--verbose       Log more (including files it decided to keep, and why)
```

With systemd, logs go to `journalctl -u zonezap`; otherwise to `/var/log/zonezap.log`.

## How it works

The WSL file server turns every Windows alternate data stream it receives into a regular Linux file called `name:stream`. ZoneZap puts one fanotify mark on each local filesystem (`FAN_MARK_FILESYSTEM` with `FAN_REPORT_DFID_NAME`, Linux 5.9+), so it sees every file created anywhere on the disk without adding a watch per folder. Most events are dismissed after a string compare on the name. For a matching name it waits until the writer closes the file (or 3 seconds pass), opens the parent directory by handle, checks the rules above, saves the xattr and calls `unlinkat`. It needs root because filesystem-wide fanotify marks require `CAP_SYS_ADMIN`.

It waits for the close, rather than deleting on create, so the Windows copy never sees an error: in testing, every `Copy-Item`, `cmd copy` and `robocopy` reported success and every copied file arrived intact.

## Why not just…

- **The `SaveZoneInformation` policy / registry key:** stops Windows from recording where any download came from, on every drive, which weakens SmartScreen and Office's Protected View. It also only affects new downloads; files already marked still bring their stream with them.
- **Adding `wsl.localhost` to Local intranet sites:** reported to work for some people and not others in #7456, and it lowers the security zone of everything served from WSL.
- **One-off cleaners** (`find ~ -name '*:Zone.Identifier' -delete`, scripts like `unzone`, wslkit's `fix zone`): they work, but the files stay around until the next run, and a sweep walks your whole home folder each time. `zonezap clean` does the same one-off sweep (staying on one filesystem), and the daemon removes new ones as they appear.

## Tested on

Windows 11 with WSL 2.7.13 and kernel 6.18.33, on Alpine 3.24 (no systemd, `wsl.conf` boot command, busybox `wget`) and Ubuntu 24.04 (systemd service, `curl`), both installed through `install.sh`:

- `Copy-Item`, `cmd copy` and `robocopy` into the distro: every `:Zone.Identifier` removed about 1 ms after the copy finished, the copy reporting success, and the zone info kept as an xattr;
- Explorer's own copy engine (the Shell `CopyHere` path, as used by paste and drag and drop) into both distros: no error dialogs, no leftovers, and the original URL kept. Explorer writes the stream up to three times per file (61 zero bytes first, then the real block, then a bare `ZoneId=3` re-mark); ZoneZap leaves the zero-filled stage alone and doesn't let the bare re-mark overwrite the fuller info it already saved;
- 2,000 files in 20 folders copied with `robocopy` and again with `Copy-Item`: all 4,000 files arrived intact, 0 leftovers;
- autostart after `wsl --terminate` and after WSL shut an idle distro down;
- the safety rules: look-alike files with other content, directories and symlinks named `*:Zone.Identifier` were all kept;
- `clean` (and `--dry-run`) on old leftovers, as root and as a normal user on their own home;
- the installer refusing a binary with a wrong checksum, and `uninstall` restoring `wsl.conf` and removing the systemd unit.

Not tested yet: Debian, Fedora and openSUSE distros, arm64 hardware (the arm64 build is compiled and self-tested by CI on an arm64 runner), and the Docker Desktop distro.

## FAQ

**Is it safe to delete these files?** Yes. They're copies of a Windows-only marker. Nothing on Linux reads them, and the original stream is still on the Windows copy of the file (if it still exists). ZoneZap keeps the content as an xattr anyway.

**Does it slow anything down?** It reads the fanotify queue and compares file names; there are no per-folder watches and no polling of the disk. While idle it uses no CPU and under 200 KB of memory. During an 8.5-second `robocopy` of 2,000 marked files it used 0.27 s of CPU in total (about 3% of one core), most of it on the 2,000 removals.

**I copied with Explorer and the log shows two removals per file.** That's expected: Explorer writes the stream, then re-marks the copy a moment later, so ZoneZap removes it twice. The zone info with the URL is kept.

**WSL 1?** No: WSL 1 has no fanotify. **Docker Desktop's distro?** It's managed by Docker; files copied into containers through it aren't covered.

**Build from source:**

```sh
make test && make        # static build with musl: apk add build-base linux-headers
```

## License

MIT. Made by [BoazCohenJ](https://github.com/BoazCohenJ).
