# The contract between Ghost Tooth UI and ghost-toothAPI

Everything in this file was read out of `ghost-toothAPI.elf` (its `.rodata`
string table and the callers of each string in `bt.c`/`main.c`), not guessed from
behaviour. If the payload is ever changed, this is the document to update first —
`src/gt_log.c` mirrors it line for line.

## What a payload is, and what it is not

A PS5 payload is a freestanding ELF loaded into a running system: no install
step, no service, no daemon, nothing listening for commands. It runs until it
exits, and it dies with the console. Two consequences shape this project:

* **There is nothing to "talk to".** ghost-toothAPI opens no socket and reads no
  FIFO; the only environment variable it looks at is `LD_LIBRARY_PATH`. The
  interface it *does* have is a config file it parses at start and a log it
  appends to while running. So Ghost Tooth UI is a *second* payload: it owns a
  web server and a UI, and it manipulates those files plus one process
  (re)launch.
* **Nothing can be installed "for later" except a tile.** The one persistent
  thing a payload can create through the system services is an AppInst title
  registration, which is what the Media-tab entry is. The payload itself still
  has to be loaded after each reboot.

## Paths

`ghost-toothAPI` derives everything from its data directory, which this project
calls the *root*:

| file | written by | read by | notes |
| --- | --- | --- | --- |
| `<root>/headset.ini` | the UI (or a text editor) | the payload, once, at start | the headset pick |
| `<root>/ghost-toothAPI.log` | the payload | the UI (tailing) | `HH:MM:SS <message>`, flushed per line |
| `<root>/ghost-toothAPI.lock` | the payload (`flock`) | the UI | liveness probe |
| `<root>/bond.key` | the payload | the payload | 16-byte link key + 1 byte key type |
| `<root>/peer.name` | the payload | the payload | the name that key belongs to |
| `<root>/ui-devices.txt` | the UI | the UI | device cache, survives a UI restart |
| `<root>/ghost-toothUI.pid` | the UI (host builds) | the UI | dev only; the console has no such line |

On a console the root is `/data/ghost-toothAPI/` (the payload's own
`mkdir(dir, 0755)` of that path is what creates it).

`headset.ini` is read line by line: a line whose key is `address` and whose
value parses as six hex pairs is a pin; a line whose key is `name` is a
substring filter. `;` and `#` start a comment, and **a commented line never
selects anything** — which is why the shipped template can carry commented
examples, and why the UI rewrites only the active lines and keeps your notes.

## Liveness

The payload keeps an exclusive `flock(LOCK_EX)` on `ghost-toothAPI.lock` for as
long as it owns the radio. The UI therefore asks "is it running?" by trying
`flock(LOCK_EX | LOCK_NB)` on the same file: if the call fails, somebody holds
it. There is no `pid:` line in the file log (the payload writes that one only to
the kernel log), so stopping means finding the process by name through
`sysctl(KERN_PROC_PROC)` and signalling it.

## Log format

The payload opens the log with `fopen(path, "ab")` and writes
`strftime("%H:%M:%S")` + `" "` + the message + `'\n'`, then `fflush()`. Two
things follow: the file is never truncated (so the UI tails with a byte cursor
and resets it only when the file *shrinks*), and lines are complete as soon as
they appear (so live tailing works without waiting for a buffer to fill).

## Strings the UI parses

These are the exact formats, with the C format specifiers the payload uses. The
UI matches a prefix, then extracts fields; an unrecognised line is still shown in
the log panel verbatim, which is what makes an unknown future payload
debuggable instead of silently misreported.

### start-up and configuration
```
ghost-toothAPI %s                                  → banner (version)
pid:%d                                             → pid, when present
config: address %s                                 → echo of the pin
config: name '%s'                                  → echo of the filter
config: bad address '%s'                           → the pin was unusable
radio ready
radio busy - already in use                        → error
another copy is already running                     → error, and "running"
bond key %s                                        → pairing state
forgetting the stored key; pair the headset again   → from "Forget pairing"
peer name %s                                       → paired peer
```

### scanning
```
searching for a headset in pairing mode              → state: scanning
no headphones yet, scanning again                    → state: scanning
no headphones found (TVs are ignored)                → state: failed
ghost-toothAPI: no headphones found - pairing mode?  → state: failed (toast)
```

### discovery — the device table
```
found %s '%s' (class %06x rssi %d)%s     the %s suffix is " [ignored]"
ignored %s '%s' class %06x                same device, skipped for good
trying %s '%s' (score %d)                 the scoring pass
headset paired before: %s
no device named '%s'; picking the most likely headphones
```
A `found` line is one row in the UI's list: address, name, class of device, RSSI,
and whether the payload marked it `[ignored]`. `trying` contributes the score and
nothing else (it must not move the "seen at" timestamp). The payload's own
ranking adds +240 for sennheiser, +220 bose, +200 sony, +180 marshall/jbl,
+170 edifier/beyerdynamic/akg/auracast, +120 for "headphones"-ish names and up to
+32 for signal, clamped at 512; a *pinned* address skips that ranking entirely,
which is the point of the Connect button.

### link and audio
```
connecting  %s                            note the two spaces
waiting for audio
no stored key: pairing
paired with %s, key saved
connection complete: status %#04x handle %#05x
connection failed: status %#04x%s
connection: no answer (try %d)
could not connect - see log               → state: failed
linked, audio setup failed - see log       → state: failed
audio  %s                                  → state: streaming (clears the error)
headset connected by itself                → state: streaming
headset connecting to us by itself
avdtp: streaming SBC %d Hz, bitpool %d     → codec + bitpool
capture: running, %d Hz %s                 → fallback codec line
stream: %d packets, %d capture records, queue %d ms, …
disconnected: status %#04x reason %#04x    → state: disconnected
switch headset off to stop
```

### exit
```
stopped
ghost-toothAPI done
ghost-toothAPI: turn the headset off and on, then try again
```

The state machine is deliberately **not** last-line-wins: `failed` and
`disconnected` always win, `streaming` once reached is never downgraded (the
payload logs `waiting for audio` *after* `audio …`, and a naive machine would
flicker), and `scanning`/`connecting` only apply while audio is not flowing. A
fresh `ghost-toothAPI <version>` banner clears the packet counter, codec,
address and error of the previous run, because mixing two runs is worse than
showing nothing.

## Choosing a headset

There are three modes, and the payload itself is what decides:

| mode | `headset.ini` | payload behaviour |
| --- | --- | --- |
| address | `address=AA:BB:CC:DD:EE:FF` | inquiry, then page that MAC; no ranking |
| name | `name=<substring>` | inquiry, first name match wins, else best score |
| auto | *(no active line)* | best score among non-ignored devices |

If a pin does not answer, the payload retries three times with a 1000 ms gap and
a 32000 ms page timeout, then gives up with `could not connect - see log`. It
never falls back to a *different* device when the pin was an address, which is
exactly the guarantee the picker is for.

A name filter, by contrast, *does* fall back (`no device named '%s'; picking the
most likely headphones`), so a name in the UI is a preference, not a promise.

## Automatic start and stop

The tile opens the UI; the UI needs the payload running to have anything to show.

* **Start.** The UI connects to `127.0.0.1:9021` (elfldr's socket server) and
  sends one line:
  `file:/data/ghost-toothAPI/ghost-toothAPI.elf?pipe=0\n`.
  `?pipe=0` matters: without it the loader hands the payload its socket as
  stdin and waits, which would serialise every restart behind a dead connection.
  If nothing answers, `capabilities.loader` goes false and the UI says so: the
  choice is still saved, the *user* starts the payload.
* **Publish.** If `ghost-toothAPI.elf` is missing from the root, the embedded copy
  (`EMBED_PAYLOAD=1`) is written to `<root>/ghost-toothAPI.elf` first, so the
  loader has something to open.
* **Stop.** SIGTERM to the pid `sysctl` reports for the payload's name, then
  SIGKILL after 1.2 s, then "turn the headset off" (the payload exits itself when
  the link drops). `-1` from the kill means the credentials were refused.
* **Auto-stop after an op.** Off by default it leaves a running payload alone;
  `--no-autostop` is how you do that when the loader, not the UI, owns the
  process. `--no-silent-scan` similarly hands the radio back to the payload's own
  judgement.

## The Media tile

`param.json`, staged in `/user/app/GTTH00001/sce_sys/` — exactly these fields:

```json
{
  "applicationCategoryType": 65536,
  "titleId": "GTTH00001",
  "deeplinkUri": "http://127.0.0.1:8899/",
  "localizedParameters": {
    "defaultLanguage": "en-US",
    "en-US": {"titleName": "Ghost Tooth"}
  }
}
```

`65536` is the shell's category mask for the **Media** tab — the value the
other `/data`-installing homebrew installers use for a web-app tile. (`0` lands
in the game list; the values real titles carry, `65792` for a disc player and
`66048` for a signed web app, mix extra capability bits into the same field.)
`deeplinkUri` is what the tile opens, and it is written with the *bound* port,
so a UI that had to walk up to a free port still points at itself. No
`contentId`, no `masterVersion`, no `appVersion`: those belong to the PKG
format, and a directory registered through AppInst does not need them.

`icon0.png` is the 512x512 PNG in `assets/`, generated by `tools/gt-icon.py`
and checked by `tools/gt-check-icon.py` in CI.

Registration order, and why each step is there:

1. `sceAppInstUtilInitialize()`.
2. Stage `param.json` + `icon0.png`: read back and compare first (so a refresh
   that changes nothing stays cheap), write with `mkstemp` → `fsync` → `rename`
   → `fsync` of the parent directory, read back again. A half-written
   `param.json` registers a tile the shell then refuses to show.
3. If the staged tree is byte-identical to what was already there *and* no
   `.pending` marker exists next to the title directory, stop — the tile is up
   to date.
4. Otherwise write `/user/app/.GTTH00001-pending` (outside `sce_sys`, so the
   shell never validates it), then `sceAppInstUtilAppUnInstall("GTTH00001")`
   — re-registering an existing title is not reliable on every firmware, and a
   repeat `AppInstallAll` without the uninstall can simply drop the tile.
5. `AppInstallTitleDir("GTTH00001", "/user/app/", 0)`, resolved by NID
   (`Wudg3Xe3heE`) through `kernel_dynlib_handle`/`kernel_dynlib_resolve`
   because the SDK ships no header for it; falls back to
   `sceAppInstUtilAppInstallAll(0)`. Up to three attempts, 250 ms apart, with a
   re-read of `param.json` between them (storage can drop the staging under
   load, and retrying the registry half against missing files is how a tile
   disappears).
6. `sceAppInstUtilTerminate()` — this payload is long-lived and should not hold
   the installer busy — and finally remove the marker. A crash before step 6
   leaves the marker, so the *next* start redoes the registration instead of
   believing the unchanged files.

**No `eboot.bin`, and no `system_ex` remount.** A `deeplinkUri` tile is opened
by the shell itself, so the launcher's eboot does not have to be copied next to
it and `/system_ex` never has to be remounted read-write — the part of the
`/system_ex/app/NPXS40106` approach that breaks on new firmware. The price is
that the tile can only open a URL, which is exactly what this UI is.

The tile is named `Ghost Tooth` in one language on purpose: the *title* stays
stable while the UI itself switches between English and Persian, and adding
localizations to `param.json` means re-registering the title on every language
toggle.

`--uninstall-tile` runs `AppUnInstall` + `Terminate` and nothing else, so the
tile can be removed without touching the payload's data files.

## What would break this

* A payload that renames its log file or drops the `HH:MM:SS ` prefix → the UI
  shows raw lines and no state; scanning still "works" (it is the payload's own
  pass) but the device list empties.
* A payload that no longer `flock`s its lock file → liveness reads "stopped"
  permanently; every action still writes config, restart is the only loss.
* A payload that gains a real control socket → delete `gt_log.c`'s parser and use
  it. Everything above the parser (tile, HTTP, UI, op queue) stays.
