**Install it inside your WSL distro with one line:**

```sh
curl -fsSL https://boazcohenj.github.io/ZoneZap/install.sh | sudo sh
```

The installer downloads the binary for your CPU from this release, checks the `.sha256`, copies it to `/usr/local/bin/zonezap` and starts it with the distro (a systemd service, or the `[boot]` command in `/etc/wsl.conf` when systemd is off). Then run `sudo zonezap clean` once to remove the old `:Zone.Identifier` files you already have, and `sudo zonezap self-test` to watch a real Windows copy get cleaned up.

The binaries are static (musl), about 100 KB, with no dependencies. They were compiled by GitHub Actions from the tagged source; check them with `sha256sum -c zonezap-linux-*.sha256`. Undo everything with `sudo zonezap uninstall`.
