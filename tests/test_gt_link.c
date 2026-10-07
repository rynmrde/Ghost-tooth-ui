/* tests/test_gt_link.c - host unit tests for the ghost-toothAPI bridge.
 *
 * Every message string used here was taken out of ghost-toothAPI.elf itself,
 * so a passing test means the UI really does understand the payload's output.
 *
 *   make test-unit
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;
static int checks;

static void
expect_int(const char *what, long got, long want) {
  checks++;
  if(got != want) {
    failures++;
    printf("FAIL %s: got %ld want %ld\n", what, got, want);
  }
}

static void
expect_str(const char *what, const char *got, const char *want) {
  checks++;
  if(!got || strcmp(got, want)) {
    failures++;
    printf("FAIL %s: got '%s' want '%s'\n", what, got ? got : "(null)", want);
  }
}

static void
expect_true(const char *what, int v) {
  checks++;
  if(!v) {
    failures++;
    printf("FAIL %s: expected true\n", what);
  }
}

/* deliberately short: every derived path must fit in PATH_MAX by construction */
static char root[256];

/* counts uncommented "key=" lines, the way the payload's parser reads them */
static int
count_active_key(const char *text, const char *key) {
  int n = 0;
  size_t klen = strlen(key);
  const char *p = text;

  while(p && *p) {
    if(!strncmp(p, key, klen) && p[klen] == '=') {
      n++;
    }
    p = strchr(p, '\n');
    if(!p) {
      break;
    }
    p++;
    while(*p == ' ' || *p == '\t') {
      p++;
    }
    if(*p == ';' || *p == '#') {
      p = strchr(p, '\n');
      if(!p) {
        break;
      }
      p++;
    }
  }
  return n;
}

static void
make_root(void) {
  char tmpl[64];

  /* a fresh directory per run: no test can be poisoned by leftovers, and no
   * shell is needed to clean one up */
  snprintf(tmpl, sizeof(tmpl), "/tmp/ghost-tooth-ui-XXXXXX");
  if(!mkdtemp(tmpl)) {
    perror("mkdtemp");
    exit(1);
  }
  gt_copy_str(root, sizeof(root), tmpl);
}

static void
write_log(const char *text) {
  char path[PATH_MAX];
  FILE *f;

  snprintf(path, sizeof(path), "%s/ghost-toothAPI.log", root);
  f = fopen(path, "wb");
  if(!f) {
    perror("log");
    exit(1);
  }
  fputs(text, f);
  fclose(f);
}

static gt_ctx_t *
new_ctx(void) {
  static gt_ctx_t ctx;
  gt_device_t *d;
  int i;

  memset(&ctx, 0, sizeof(ctx));
  pthread_mutex_init(&ctx.lock, 0);
  gt_copy_str(ctx.cfg.root, sizeof(ctx.cfg.root), root);
  ctx.cfg.scan_seconds = 5;
  ctx.mode = GT_MODE_AUTO;
  ctx.link = GT_LINK_IDLE;
  ctx.op_pending = GT_OP_NONE;
  ctx.payload_pid = -1;
  for(i = 0, d = ctx.devices; i < GT_MAX_DEVICES; i++, d++) {
    (void)d;
  }
  gt_paths_resolve(&ctx);
  return &ctx;
}

/* --------------------------------------------------------------- the tests */

static void
test_addr(void) {
  char out[64];

  expect_true("valid mac", gt_addr_valid("AA:BB:CC:DD:EE:FF"));
  expect_int("short mac rejected", gt_addr_valid("AA:BB:CC:DD:EE"), 0);
  expect_int("garbage rejected", gt_addr_valid("hello world"), 0);
  expect_int("wrong colon places", gt_addr_valid("AABB:CC:DD:EE:FF:00"), 0);
  expect_int("lowercase mac", gt_addr_valid("aa:bb:cc:dd:ee:ff"), 1);
  expect_int("normalize dashes", gt_addr_normalize("aa-bb-cc-dd-ee-ff", out, sizeof(out)), 0);
  expect_str("normalize upper", out, "AA:BB:CC:DD:EE:FF");
  expect_int("normalize spaces", gt_addr_normalize(" 0a 1b 2c 3d 4e 5f", out, sizeof(out)), 0);
  expect_str("normalize spaces result", out, "0A:1B:2C:3D:4E:5F");
  expect_int("reject 11 nibbles", gt_addr_normalize("0A:1B:2C:3D:4E:5", out, sizeof(out)), -1);
}

static void
test_kind(void) {
  expect_int("tv by name", gt_guess_kind("BRAVIA 4K 2023", "060104"), GT_KIND_TV);
  /* a name the tables do not know stays unknown: the payload's own "[ignored]"
   * marker, not this hint, is what flags a television */
  expect_int("unknown name", gt_guess_kind("Living Room", "060104"), GT_KIND_UNKNOWN);
  expect_int("headphones by name", gt_guess_kind("WH-1000XM4", "5a020c"), GT_KIND_HEADPHONES);
  expect_int("earbuds", gt_guess_kind("Galaxy Buds3 Pro", "5a020c"), GT_KIND_HEADPHONES);
  expect_int("soundbar beats the brand", gt_guess_kind("Bose TV Soundbar", "5a020c"),
             GT_KIND_SPEAKER);
  expect_int("av class", gt_guess_kind("x", "000420"), GT_KIND_HEADPHONES);
}

static void
test_log_parse(void) {
  gt_ctx_t *ctx = new_ctx();
  gt_device_t *d;
  char buf[8192];

  /* Verbatim payload output: "%H:%M:%S " prefix, then the format strings from
   * ghost-toothAPI.elf ("found %s '%s' (class %06x rssi %d)%s", "trying %s
   * '%s' (score %d)", ...). */
  strcpy(buf,
         "10:00:01 ghost-toothAPI 0.9.4\n"
         "10:00:01 radio ready\n"
         "10:00:01 searching for a headset in pairing mode\n"
         "10:00:02 found 30:3A:64:11:22:33 'WH-1000XM4' (class 5a020c rssi -47)\n"
         "10:00:02 found C4:62:6B:DD:EE:FF 'BRAVIA 4K 2023' (class 060104 rssi -55) [ignored]\n"
         "10:00:03 ignored C4:62:6B:DD:EE:FF 'BRAVIA 4K 2023' class 060104\n"
         "10:00:03 found 6C:B3:11:77:88:99 'Galaxy Buds3 Pro' (class 5a020c rssi -63)\n"
         "10:00:04 found A4:83:E7:44:55:66 'QC45' (class 5a020c rssi -58)\n"
         "10:00:05 trying A4:83:E7:44:55:66 'QC45' (score 118)\n"
         "10:00:05 trying 30:3A:64:11:22:33 'WH-1000XM4' (score 132)\n"
         "10:00:06 connecting  30:3A:64:11:22:33\n"
         "10:00:07 paired with 30:3A:64:11:22:33, key saved\n"
         "10:00:08 avdtp: streaming SBC 48000 Hz, bitpool 40\n"
         "10:00:08 audio  30:3A:64:11:22:33\n"
         "10:00:09 stream: 375 packets, 375 capture records, queue 9 ms, trimmed 0, overruns 0, restarts 0, 48000 Hz, credits 6, completions assumed 0, reports missing 0\n");
  write_log(buf);

  ctx->log_cursor = 0;
  ctx->scan_active = 1;              /* only a scan the user asked for counts */
  gt_log_pump(ctx);

  expect_int("four devices", ctx->device_count, 4);
  /* the "ignored" line is a note about an already counted device */
  expect_int("scan found four devices", ctx->scan_found, 4);
  d = gt_device_find(ctx, "30:3A:64:11:22:33");
  expect_true("headphones present", d != 0);
  if(d) {
    expect_str("name parsed", d->name, "WH-1000XM4");
    expect_str("class parsed", d->cls, "5a020c");
    expect_int("rssi parsed", d->rssi, -47);
    expect_int("score parsed", d->score, 132);
    expect_int("not ignored", d->ignored, 0);
    expect_int("kind headphones", d->kind, GT_KIND_HEADPHONES);
    expect_str("timestamp", d->time, "10:00:02");
  }
  d = gt_device_find(ctx, "C4:62:6B:DD:EE:FF");
  expect_true("tv present", d != 0);
  if(d) {
    expect_int("tv ignored", d->ignored, 1);
    expect_int("tv kind", d->kind, GT_KIND_TV);
  }
  expect_str("link streaming", gt_link_name(ctx->link), "streaming");
  expect_str("payload addr", ctx->payload_addr, "30:3A:64:11:22:33");
  expect_str("codec", ctx->codec, "SBC 48000 Hz");
  expect_int("bitpool", ctx->bitpool, 40);
  expect_int("packets", ctx->packets, 375);
  expect_int("queue ms", ctx->queue_ms, 9);
  expect_int("log ring lines", ctx->line_count, 15);
  expect_str("last log line", ctx->lines[(ctx->line_head - 1 + GT_MAX_LINES) % GT_MAX_LINES],
             "stream: 375 packets, 375 capture records, queue 9 ms, trimmed 0, "
             "overruns 0, restarts 0, 48000 Hz, credits 6, completions assumed 0, "
             "reports missing 0");

  /* a truncated tail line must not be parsed until its newline arrives */
  {
    FILE *f = fopen(ctx->cfg.log_path, "ab");

    ctx->scan_active = 0;

    fputs("10:00:10 found 11:22:33:44:55:66 'Half Write", f);
    fclose(f);
    gt_log_pump(ctx);
    expect_int("half line not parsed yet", ctx->device_count, 4);
    expect_int("a connect pass does not add to the scan count", ctx->scan_found, 4);
    f = fopen(ctx->cfg.log_path, "ab");
    fputs("d' (class 5a020c rssi -90)\n", f);
    fclose(f);
    gt_log_pump(ctx);
    expect_int("completed line parsed", ctx->device_count, 5);
    expect_str("name across chunks", gt_device_find(ctx, "11:22:33:44:55:66")->name,
               "Half Writed");
  }

  /* a truncated log (console reboot) restarts from the beginning */
  write_log("10:05:00 radio busy - already in use\n");
  gt_log_pump(ctx);
  expect_str("error state from busy", gt_link_name(ctx->link), "failed");
  expect_true("busy recorded", strstr(ctx->last_error, "radio busy") != 0);
}

static void
test_state_words(void) {
  struct { const char *line; const char *state; } cases[] = {
    {"10:00:00 radio ready", "scanning"},
    {"10:00:00 searching for a headset in pairing mode", "scanning"},
    {"10:00:00 no headphones yet, scanning again", "scanning"},
    {"10:00:00 connecting  AA:BB:CC:DD:EE:FF", "connecting"},
    {"10:00:00 waiting for audio", "connecting"},
    {"10:00:00 audio  AA:BB:CC:DD:EE:FF", "streaming"},
    {"10:00:00 headset connected by itself", "streaming"},
    {"10:00:00 capture: running, 44100 Hz stereo", "streaming"},
    {"10:00:00 disconnected: status 0x16 reason 0x13", "disconnected"},
    {"10:00:00 ghost-toothAPI done", "disconnected"},
    {"10:00:00 could not connect - see log", "failed"},
    {"10:00:00 no headphones found (TVs are ignored)", "failed"},
    {"10:00:00 avdtp: open refused", "failed"}
  };
  size_t i;

  for(i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    gt_ctx_t *ctx = new_ctx();
    char text[512];
    FILE *f;

    snprintf(text, sizeof(text), "%s\n", cases[i].line);
    write_log(text);
    ctx->log_cursor = 0;
    gt_log_pump(ctx);
    expect_str(cases[i].state, gt_link_name(ctx->link), cases[i].state);
    f = fopen(ctx->cfg.log_path, "rb");
    if(f) {
      fclose(f);
    }
  }
}

static void
test_config_echo(void) {
  gt_ctx_t *ctx = new_ctx();
  FILE *f;

  /* The ini is the configuration; an echo from the payload only ever confirms
   * it, so a stale echo must not move the UI back to the previous pick. */
  gt_ini_save(ctx, GT_MODE_ADDRESS, "11:22:33:44:55:66");
  f = fopen(ctx->cfg.log_path, "wb");
  fputs("10:00:00 config: address DE:AD:BE:EF:00:01\n", f);
  fputs("10:00:00 config: name 'bose'\n", f);
  fclose(f);
  ctx->log_cursor = 0;
  gt_log_pump(ctx);
  expect_int("echo does not override the ini", ctx->mode, GT_MODE_ADDRESS);
  expect_str("echo does not override the address", ctx->want_addr,
             "11:22:33:44:55:66");

  /* a value the payload refused is worth a warning, though */
  f = fopen(ctx->cfg.log_path, "wb");
  fputs("10:00:00 config: bad address 'zz:zz'\n", f);
  fclose(f);
  ctx->log_cursor = 0;
  gt_log_pump(ctx);
  expect_true("refused address warns",
              strstr(ctx->last_error, "refused") != 0);
}

static void
test_ini(void) {
  gt_ctx_t *ctx = new_ctx();
  char buf[4096];
  size_t got = 0;
  char path[PATH_MAX];

  snprintf(path, sizeof(path), "%s/headset.ini", root);

  /* absent file: a fresh template is written, no selection */
  unlink(path);
  gt_ini_load(ctx);
  expect_int("no ini means auto", ctx->mode, GT_MODE_AUTO);

  expect_int("save address", gt_ini_save(ctx, GT_MODE_ADDRESS, "aa:bb:cc:dd:ee:ff"), 0);
  expect_int("mode after save", ctx->mode, GT_MODE_ADDRESS);
  expect_str("address after save", ctx->want_addr, "AA:BB:CC:DD:EE:FF");
  gt_read_file(path, buf, sizeof(buf), &got);
  expect_true("address line present", strstr(buf, "address=AA:BB:CC:DD:EE:FF") != 0);
  expect_true("template comment kept", strstr(buf, "optional headset pick") != 0);

  /* re-pinning replaces the previous active line, comments survive */
  {
    FILE *f = fopen(path, "ab");

    fputs("; my own note\n", f);
    fclose(f);
  }
  expect_int("save other address", gt_ini_save(ctx, GT_MODE_ADDRESS, "00:11:22:33:44:55"), 0);
  gt_read_file(path, buf, sizeof(buf), &got);
  expect_true("user note kept", strstr(buf, "; my own note") != 0);
  expect_true("old pin gone", !strstr(buf, "address=AA:BB:CC:DD:EE:FF"));
  expect_true("new pin present", strstr(buf, "address=00:11:22:33:44:55") != 0);
  expect_int("one active address line", count_active_key(buf, "address"), 1);
  expect_int("no active name line", count_active_key(buf, "name"), 0);

  /* a commented line is never a selection */
  {
    FILE *f = fopen(path, "wb");

    fputs("; address=11:11:11:11:11:11\n; name=beats\n", f);
    fclose(f);
  }
  gt_ini_load(ctx);
  expect_int("comments ignored", ctx->mode, GT_MODE_AUTO);

  /* name mode, and the characters that must be refused */
  expect_int("save name", gt_ini_save(ctx, GT_MODE_NAME, "soundcore"), 0);
  expect_int("name mode", ctx->mode, GT_MODE_NAME);
  expect_str("name value", ctx->want_name, "soundcore");
  expect_int("reject newline injection", gt_ini_save(ctx, GT_MODE_NAME, "a\naddress=00:00:00:00:00:01"), -1);
  expect_int("reject '='", gt_ini_save(ctx, GT_MODE_NAME, "x=y"), -1);
  expect_int("reject empty name", gt_ini_save(ctx, GT_MODE_NAME, ""), -1);
  expect_int("reject bad address", gt_ini_save(ctx, GT_MODE_ADDRESS, "nope"), -1);

  /* back to automatic: no active lines left */
  expect_int("save auto", gt_ini_save(ctx, GT_MODE_AUTO, 0), 0);
  gt_read_file(path, buf, sizeof(buf), &got);
  expect_int("auto clears the pins", count_active_key(buf, "address"), 0);
  expect_int("auto clears the name", count_active_key(buf, "name"), 0);
  gt_ini_load(ctx);
  expect_int("auto mode loaded", ctx->mode, GT_MODE_AUTO);
}

static void
test_cache_roundtrip(void) {
  gt_ctx_t *ctx = new_ctx();
  gt_ctx_t *other;
  char text[] =
    "10:00:00 found 30:3A:64:11:22:33 'WH-1000XM4' (class 5a020c rssi -47)\n"
    "10:00:01 found C4:62:6B:DD:EE:FF 'BRAVIA 4K 2023' (class 060104 rssi -55) [ignored]\n";
  FILE *f;

  f = fopen(ctx->cfg.log_path, "wb");
  fputs(text, f);
  fclose(f);
  ctx->log_cursor = 0;
  gt_log_pump(ctx);
  gt_cache_save(ctx);

  other = new_ctx();
  gt_cache_load(other);
  expect_int("cache restored rows", other->device_count, 2);
  {
    gt_device_t *d = gt_device_find(other, "30:3A:64:11:22:33");

    expect_true("cache row", d != 0);
    if(d) {
      expect_str("cache name", d->name, "WH-1000XM4");
      expect_int("cache rssi", d->rssi, -47);
      expect_str("cache time", d->time, "10:00:00");
    }
    d = gt_device_find(other, "C4:62:6B:DD:EE:FF");
    expect_true("cache ignored", d && d->ignored);
  }
}

static void
test_json(void) {
  gt_ctx_t *ctx = new_ctx();
  static char buf[65536];
  gt_jb_t jb;
  char text[] = "10:00:00 found 30:3A:64:11:22:33 'Bose \"QC\" \\ x' (class 5a020c rssi -47)\n";
  FILE *f;

  f = fopen(ctx->cfg.log_path, "wb");
  fputs(text, f);
  fputs("10:00:01 found 6C:B3:11:77:88:99 'Galaxy Buds3 Pro' (class 5a020c rssi -63)\n", f);
  fclose(f);
  ctx->log_cursor = 0;
  gt_log_pump(ctx);

  gt_jb_begin(&jb, buf, sizeof(buf));
  gt_jb_obj_open(&jb, 0);
  gt_state_json(ctx, &jb);
  gt_devices_json(ctx, &jb);
  gt_log_json(ctx, &jb, 1);
  gt_op_json(ctx, &jb);
  gt_jb_obj_close(&jb);

  expect_int("json fits", gt_jb_error(&jb), 0);
  expect_true("quotes escaped", strstr(buf, "Bose \\\"QC\\\" \\\\ x") != 0);
  expect_true("device array", strstr(buf, "\"devices\":[{") != 0);
  expect_true("link state idle", strstr(buf, "\"state\":\"idle\"") != 0);
  expect_true("payload block", strstr(buf, "\"payload\":{") != 0);
  expect_true("config block", strstr(buf, "\"config\":{") != 0);
  expect_true("log block", strstr(buf, "\"log\":{") != 0);
  expect_true("balanced tail", buf[strlen(buf) - 1] == '}');
  expect_true("log array elements separated", strstr(buf, "}{") == 0);
  expect_true("device array elements separated", strstr(buf, "}]") != 0);
  {
    int open = 0;
    int close = 0;
    char *p = buf;

    while(*p) {
      if(*p == '{') open++;
      if(*p == '}') close++;
      p++;
    }
    expect_int("braces balanced", open - close, 0);
  }
}

#define RUN(fn) do {                                     \
    printf("== %s\n", #fn);                              \
    fflush(stdout);                                      \
    fn();                                                \
  } while(0)

int
main(void) {
  setvbuf(stdout, 0, _IONBF, 0);
  make_root();
  RUN(test_addr);
  RUN(test_kind);
  RUN(test_log_parse);
  RUN(test_state_words);
  RUN(test_config_echo);
  RUN(test_ini);
  RUN(test_cache_roundtrip);
  RUN(test_json);
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
