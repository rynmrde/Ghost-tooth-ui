# Ghost Tooth UI

A PS5 payload that puts a **Bluetooth headset picker on the Media tab**, so the
audio payload [ghost-toothAPI](https://github.com/rynnmarde/ghost-toothAPI) can
be driven from the dashboard instead of from a loader every single time.

You scan, you tap the headset you want, and its address is written to
`headset.ini` for `ghost-toothAPI` to page. Nothing inside `ghost-toothAPI.elf`
is patched, repacked or rebuilt — this is a *separate* payload that only reads
and writes the files the other one already uses
([why it works that way](docs/INTEGRATION.md#what-a-payload-is-and-what-it-is-not)).

```
PS5 ── dashboard ── Media tab ──▶ [Ghost Tooth] ──▶ local web UI (this payload)
                                          │
                                          │ writes /data/ghost-toothAPI/headset.ini
                                          │ asks the payload loader to (re)start
                                          ▼
                              ghost-toothAPI.elf ──▶ USB BT dongle ──▶ headset
```

## What it does

* **Register itself as a Media tile.** On the first (and only) loader run it
  stages `/user/app/GTTH00001/sce_sys/{param.json,icon0.png}` and installs it
  through `AppInstallTitleDir`, with `applicationCategoryType: 65536` — the
  value the shell uses for the **Media** tab. From then on it is a tile you open
  with the controller like any other app; the loader is not needed again until
  the console reboots.
* **Serve the picker UI** on `http://127.0.0.1:8899/` (the tile's deep link) and
  on the LAN address, so you can also open it from a phone.
* **Scan for headsets** by running ghost-toothAPI's *own* inquiry pass and
  reading the `found …` lines out of its log.
* **Pick one.** A card per device: name, address, RSSI, class of device, what
  the payload made of it (headphones / TV / skipped), its score.
* **Connect.** Writes `address=AA:BB:CC:DD:EE:FF` into `headset.ini` and restarts
  ghost-toothAPI, which then pages exactly that device instead of guessing.
* **Follow the link** from the payload's log: connecting, paired, streaming
  (codec, bitpool, packet count, buffer), disconnected, and the exact error if
  it failed.
* **Stop / restart / forget pairing** — the same things the payload's files let
  you do.
* **فارسی + English**, including a right-to-left layout, switchable in the
  header.

## Why it goes through files

`ghost-toothAPI` is a payload, not a service: no socket API, no control channel,
and the only environment variable it reads is `LD_LIBRARY_PATH`. What it *does*
have is a config file it parses at start and a line-buffered log it appends to
while it runs. Those two are the interface, and this UI treats them as the single
source of truth:

| path | direction | meaning |
| --- | --- | --- |
| `/data/ghost-toothAPI/headset.ini` | write | `name=<substring>` or `address=<MAC>` picks the headset |
| `/data/ghost-toothAPI/ghost-toothAPI.log` | read | everything that happened, `HH:MM:SS <message>` |
| `/data/ghost-toothAPI/ghost-toothAPI.lock` | read | held while the payload runs → liveness |
| `/data/ghost-toothAPI/bond.key`, `peer.name` | read / delete | pairing state ("Forget pairing" unlinks both) |

The full string-by-string contract, including what happens if the payload's
messages ever change, is in [`docs/INTEGRATION.md`](docs/INTEGRATION.md).

## Getting it on the console

1. Download `ghost-tooth-ui.elf` from the
   [latest release](https://github.com/rynmrde/Ghost-tooth-ui/releases), from the
   artifacts of any [CI run](../../actions/workflows/build.yml) (see
   [Building the ELF](#building-the-elf) - both contain the exact same build), or
   build it yourself with `make ps5`. It is the *companion* payload —
   `ghost-toothAPI.elf` stays as it is.
2. Make sure `ghost-toothAPI.elf` is somewhere obvious
   (`/data/ghost-toothAPI/ghost-toothAPI.elf` is where it looks first). If it is
   not there, use the `selfcontained` build, which writes its embedded copy
   there on first run.
3. Load `ghost-tooth-ui.elf` once through your usual loader (FTP/elfldr, or
   whatever runs payloads on your setup). A toast appears telling you the tile
   is registered; the console does not need the loader again for this.
4. Open **Media → Ghost Tooth**.
5. Put the headset in pairing mode, press **Scan for devices**, choose it, press
   **Connect**.

After that, every time you want the headset: open the tile. The tile only shows
the UI; `ghost-toothAPI` itself is started by your loader when it is not already
running — see [Automatic start](docs/INTEGRATION.md#automatic-start-and-stop)
for what the tile does and does not do about that.

## Building the ELF

There are three ways to get `ghost-tooth-ui.elf`; they all produce the same
binary, and only the last two need a machine of your own.

**1 · Let CI build it.** The [`build` workflow](.github/workflows/build.yml)
runs `make ps5` in the official toolchain for every push to `main`, every pull
request and every manual run, and uploads two artifacts:

| artifact | what it is |
| --- | --- |
| `ghost-tooth-ui-ps5` | the portable ELF + `ghost-tooth-ui.elf.sha256`; needs `ghost-toothAPI.elf` already in `/data/ghost-toothAPI/` |
| `ghost-tooth-ui-ps5-selfcontained` | the same ELF with `ghost-toothAPI.elf` embedded, so the UI installs the payload itself |

```sh
gh run list --workflow build --repo <you>/Ghost-tooth-ui      # find the run
gh run download <run-id> -n ghost-tooth-ui-ps5                # the ELF
# or, to kick off a build without pushing to main:
gh workflow run build.yml --repo <you>/Ghost-tooth-ui --ref <branch>
```
On a tag (`v0.1.0`) the portable ELF is published to the release page instead.

**2 · Build it locally (Linux, one command).** `make sdk` installs the
[ps5-payload-dev](https://github.com/ps5-payload-dev/sdk) toolchain — the same
package the other homebrew payloads use, containing `prospero-clang`, `lld`,
the sysroot and the `-lSce*` stubs:

```sh
make sdk            # downloads ps5-payload-dev.tar.gz and untars it into / (sudo)
make ps5            # -> build/ghost-tooth-ui.elf
# or both ELF variants + checksums, exactly as CI does:
make dist           # -> dist/ghost-tooth-ui.elf, dist/ghost-tooth-ui-selfcontained.elf
```

If you already have the SDK somewhere - a checkout you built, or a container
that ships it - point at it instead of running `make sdk`:

```sh
git clone --recursive https://github.com/ps5-payload-dev/sdk
sudo make -C sdk DESTDIR=/opt/ps5-payload-sdk install
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk && make ps5
```

Note that a *bare* SDK checkout is not enough: `toolchain/prospero.mk`,
`bin/prospero-clang` and the sysroot are created by its `make install`, which is
why both `make sdk` and CI unpack a finished tree. `make ps5` says so if you get
this wrong, and `make check-ps5` compiles every source with the PS5 defines
against headers only, so you can validate the console-only code paths
(`sysctl`, AppInst, notifications) without a toolchain at all.

**3 · In a container of your own.** The build has no other requirement than a
Linux userland with `/opt/ps5-payload-sdk` in it, so any image works as long as
the toolchain is inside - e.g. a plain Debian container:

```sh
docker run --rm -v "$PWD":/work -w /work debian:bookworm-slim bash -c \
  'apt-get update -qq && apt-get install -y -qq curl ca-certificates tar &&
   curl -fL -o /tmp/t.tar.gz https://github.com/ps5-payload-dev/pacbrew-repo/releases/latest/download/ps5-payload-dev.tar.gz &&
   tar xf /tmp/t.tar.gz -C / && make ps5 PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk'
```
(That is exactly what the `ps5` job in CI does, on a runner that already has
`curl` and `tar`.)

### What the two ELF variants mean

`EMBED_PAYLOAD=1` (the default for a local build, since `ghost-toothAPI.elf`
sits in this repo) embeds that image in a read-only section; on first run the UI
writes it to `/data/ghost-toothAPI/ghost-toothAPI.elf` **only if none is there**,
then restarts it via the loader. `EMBED_PAYLOAD=0` leaves the file alone: the UI
still finds and restarts the payload, but it will not install it, and CI
publishes only this variant so no upstream payload is redistributed here.

Both are plain payload ELFs - load them the way you load any other, and no
`fself`/PKG step is involved (see
[docs/INTEGRATION.md](docs/INTEGRATION.md#the-media-tile) for why the tile needs
neither).

### Pushing it to the console

```sh
make deploy PS5_HOST=<ps5-ip>          # prospero-deploy to the loader on :9021
# or with any loader you already use:
cat build/ghost-tooth-ui.elf | nc <ps5-ip> 9021
```

## Using it from a shell

Every button is one `POST`, and the state is one `GET` — useful when the console
is headless:

```sh
curl -s http://<ps5-ip>:8899/api/state | jq '.link, .devices[].name'
curl -s -X POST -d '{"action":"scan"}'                      http://<ps5-ip>:8899/api/action
curl -s -X POST -d '{"action":"connect","arg":"30:3A:64:11:22:33"}' http://<ps5-ip>:8899/api/action
curl -s -X POST -d '{"action":"stop"}'                      http://<ps5-ip>:8899/api/action
# also accepted, for muscle memory:
curl -s -X POST "http://<ps5-ip>:8899/api/connect?addr=30:3A:64:11:22:33"
```

Actions: `scan`, `scan-live`, `connect`, `name`, `auto`, `apply`, `stop`,
`forget`, `tile`. `GET /api/state`, `/api/devices` and `/api/log` all refresh
from the payload's log first, so they are always as current as the log file.

## Honest limitations

* **Scanning is the payload's own inquiry pass, not a private scan.**
  ghost-toothAPI has no "scan only" mode, so a scan starts the payload pinned to
  a deliberately dead address (`02:00:00:00:00:00`), collects the `found …`
  lines, restores whatever was in `headset.ini` before, and stops it again. It
  takes ~16 s (the payload's inquiry window) and it cannot be made faster from
  here. `--no-silent-scan` skips the pin and lets the payload connect to its own
  best guess instead.
* **A device can appear unnamed.** Devices that never advertise an eir name show
  as `ghost-tooth AA:BB`; the address is always real, the label is not.
* **Stopping needs luck on a locked-down firmware.** It finds the process by
  name through `sysctl`, then SIGTERMs it; if the kernel refuses (different
  credentials and `kernel_set_ucred` unavailable) the UI tells you to turn the
  headset off, which stops the payload by itself.
* **The tile is a Media-tab registration, not a PKG.** It is created with
  AppInst on the console (`GTTH00001`); remove it again with
  `ghost-tooth-ui.elf --uninstall-tile`.
* **No audio goes through this payload.** It never touches the mic/speaker
  path; ghost-toothAPI does that, and this UI only tells it which device to
  page.

## License

GPL-3.0-or-later for everything in this repository. It does **not** contain,
link or modify `ghost-toothAPI.elf`; the optional `EMBED_PAYLOAD=1` step copies a
file you already own into the payload's own data directory, and is off by
default in CI releases.
