/* gt_log.c - consumes ghost-toothAPI's own log file and turns it into UI state.
 *
 * The payload writes one line per event to /data/ghost-toothAPI/ghost-toothAPI.log
 * in the form "HH:MM:SS <message>" (strftime "%H:%M:%S", then "%s ", then the
 * message, then '\n'), opened in append mode and flushed per line.  That log is
 * the only feedback the payload gives, so this file is where every string the
 * UI shows is matched.  Unrecognised lines are still forwarded to the log
 * panel: a payload update cannot silently hide state from the interface.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* ------------------------------------------------------------- device table */

gt_device_t *
gt_device_find(gt_ctx_t *ctx, const char *addr) {
  int i;

  for(i = 0; i < ctx->device_count; i++) {
    if(!strcmp(ctx->devices[i].addr, addr)) {
      return &ctx->devices[i];
    }
  }
  return 0;
}

/* Returns a row for addr, creating it when first seen.  When the table is
 * full the least recently sighted row is recycled, so a room full of TVs
 * cannot push the headset out of the list. */
gt_device_t *
gt_device_touch(gt_ctx_t *ctx, const char *addr) {
  gt_device_t *d = gt_device_find(ctx, addr);

  if(d) {
    d->seq = ++ctx->seq;
    d->sightings++;
    return d;
  }
  if(ctx->device_count >= GT_MAX_DEVICES) {
    int oldest = 0;
    int i;

    for(i = 1; i < ctx->device_count; i++) {
      if(ctx->devices[i].seq < ctx->devices[oldest].seq) {
        oldest = i;
      }
    }
    d = &ctx->devices[oldest];
  } else {
    d = &ctx->devices[ctx->device_count++];
  }
  memset(d, 0, sizeof(*d));
  gt_copy_str(d->addr, sizeof(d->addr), addr);
  d->score = -1;
  d->seq = ++ctx->seq;
  d->sightings = 1;
  return d;
}

/* ------------------------------------------------------------- token reader */

static const char *
gt_skip_space(const char *s) {
  while(*s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

static const char *
gt_skip_to_space(const char **pp) {
  const char *p = *pp;

  while(*p && *p != ' ' && *p != '\t') {
    p++;
  }
  *pp = p;
  return p;
}


/* Reads one bare token (address, hex class, number) up to whitespace or '('. */
static int
gt_read_token(const char **pp, char *out, size_t size) {
  const char *p = gt_skip_space(*pp);
  size_t o = 0;

  while(*p && !isspace((unsigned char)*p) && *p != '(' && o + 1 < size) {
    out[o++] = *p++;
  }
  out[o] = 0;
  *pp = p;
  return o ? 0 : -1;
}

/* Reads one 'single quoted' token, e.g. a Bluetooth device name. */
static int
gt_read_quoted(const char **pp, char *out, size_t size) {
  const char *p = gt_skip_space(*pp);
  size_t o = 0;

  if(*p != '\'') {
    out[0] = 0;
    return -1;
  }
  p++;
  while(*p && *p != '\'' && o + 1 < size) {
    out[o++] = *p++;
  }
  out[o] = 0;
  if(*p != '\'') {
    return -1;
  }
  *pp = p + 1;
  return 0;
}

__attribute__((unused)) static int
gt_read_int(const char **pp, const char *keyword, long *out) {
  const char *p = strstr(*pp, keyword);
  int ok;

  if(!p) {
    return -1;
  }
  p += strlen(keyword);
  *out = gt_parse_long(p, &ok);
  if(!ok) {
    return -1;
  }
  *pp = p;
  return 0;
}

static int
gt_startsWith(const char *s, const char *prefix) {
  size_t n = strlen(prefix);

  return !strncmp(s, prefix, n);
}

/* --------------------------------------------------------------- parsing */

static void
gt_parse_found(gt_ctx_t *ctx, const char *text, const char *stamp) {
  char addr[64] = {0};
  char name[GT_NAME_SIZE] = {0};
  char cls[16] = {0};
  const char *p = text + 5;            /* strlen("found") */
  gt_device_t *d;
  long rssi = 0;

  if(gt_read_token(&p, addr, sizeof(addr)) || !gt_addr_valid(addr)) {
    return;
  }
  gt_read_quoted(&p, name, sizeof(name));
  /* found AA:BB:CC:DD:EE:FF 'Name' (class 5a020c rssi -47) [ignored] */
  if((p = strstr(p, "class"))) {
    p += 5;
    gt_skip_to_space(&p);                 /* past the '(' if it hugs the word */
    gt_read_token(&p, cls, sizeof(cls));
  }
  if((p = strstr(p, "rssi"))) {
    p += 4;
    rssi = gt_parse_long(gt_skip_space(p), 0);
  }
  d = gt_device_touch(ctx, addr);
  if(name[0]) {
    gt_copy_str(d->name, sizeof(d->name), name);
  }
  if(cls[0]) {
    gt_copy_str(d->cls, sizeof(d->cls), cls);
  }
  if(rssi) {
    d->rssi = (int)rssi;
  }
  d->ignored = strstr(text, "[ignored]") ? 1 : 0;
  gt_copy_str(d->time, sizeof(d->time), stamp);
  d->kind = gt_guess_kind(d->name, d->cls);
  if(d->ignored && d->kind == GT_KIND_UNKNOWN) {
    d->kind = GT_KIND_TV;
  }
  if(ctx->scan_active) {
    ctx->scan_found++;
  }
}

static void
gt_parse_ignored(gt_ctx_t *ctx, const char *text, const char *stamp) {
  char addr[64] = {0};
  char name[GT_NAME_SIZE] = {0};
  char cls[16] = {0};
  const char *p = text + 7;            /* strlen("ignored") */
  gt_device_t *d;

  if(gt_read_token(&p, addr, sizeof(addr)) || !gt_addr_valid(addr)) {
    return;
  }
  gt_read_quoted(&p, name, sizeof(name));
  if((p = strstr(p, "class"))) {
    p += 5;                         /* past the word "class" itself */
    gt_read_token(&p, cls, sizeof(cls));
  }
  d = gt_device_touch(ctx, addr);
  if(name[0]) {
    gt_copy_str(d->name, sizeof(d->name), name);
  }
  if(cls[0]) {
    gt_copy_str(d->cls, sizeof(d->cls), cls);
  }
  d->ignored = 1;
  if(d->kind == GT_KIND_UNKNOWN) {
    d->kind = GT_KIND_TV;
  }
  gt_copy_str(d->time, sizeof(d->time), stamp);
}

static void
gt_parse_trying(gt_ctx_t *ctx, const char *text, const char *stamp) {
  char addr[64] = {0};
  char name[GT_NAME_SIZE] = {0};
  const char *p = text + 6;            /* strlen("trying") */
  gt_device_t *d;
  long score;

  if(gt_read_token(&p, addr, sizeof(addr)) || !gt_addr_valid(addr)) {
    return;
  }
  gt_read_quoted(&p, name, sizeof(name));
  d = gt_device_touch(ctx, addr);
  if(name[0]) {
    gt_copy_str(d->name, sizeof(d->name), name);
  }
  if(!gt_read_int(&p, "score", &score)) {
    d->score = (int)score;
  }
}

/* Extracts the address that follows a two-word prefix such as "connecting ". */
static int
gt_addr_after(const char *text, const char *prefix, char *out, size_t size) {
  const char *p = gt_skip_space(text + strlen(prefix));
  char addr[64] = {0};

  if(gt_read_token(&p, addr, sizeof(addr)) || !gt_addr_valid(addr)) {
    return -1;
  }
  gt_copy_str(out, size, addr);
  return 0;
}

static void
gt_refresh_device_flags(gt_ctx_t *ctx) {
  int i;

  for(i = 0; i < ctx->device_count; i++) {
    gt_device_t *d = &ctx->devices[i];

    d->pinned = ctx->mode == GT_MODE_ADDRESS && !strcmp(d->addr, ctx->want_addr);
    d->paired = ctx->bond_present &&
                ((!ctx->payload_addr[0] && !strcmp(d->addr, ctx->want_addr)) ||
                 !strcmp(d->addr, ctx->payload_addr) ||
                 (ctx->peer_name[0] && d->name[0] &&
                  !strcasecmp(d->name, ctx->peer_name)));
  }
}

static const char *const gt_scanning_words[] = {
  "radio ready",
  "searching for a headset",
  "no headphones yet, scanning again",
  "ghost-toothAPI: searching",
  0
};

static const char *const gt_failed_words[] = {
  "could not connect",
  "connection failed",
  "linked, audio setup failed",
  "no headphones found",
  "ghost-toothAPI: no headphones found",
  "radio busy",
  "another copy is already running",
  "encryption failed",
  "authentication failed",
  "avdtp: open refused",
  "avdtp: configuration refused",
  "sbc: init failed",
  0
};

static const char *const gt_lost_words[] = {
  "disconnected",
  "switch headset off to stop",
  "stream: ended after",
  "ghost-toothAPI done",
  "stopped",
  0
};

static const char *const gt_streaming_words[] = {
  "audio ",
  "headset connected by itself",
  "headset connecting to us by itself",
  "avdtp: streaming",
  "capture: running",
  "stream:",
  0
};

static const char *const gt_connecting_words[] = {
  "connecting",
  "waiting for audio",
  "paired with",
  "no stored key: pairing",
  "connection complete",
  0
};

static int
gt_matches(const char *text, const char *const *words) {
  int i;

  for(i = 0; words[i]; i++) {
    if(gt_startsWith(text, words[i])) {
      return 1;
    }
  }
  return 0;
}

static void
gt_apply_log_line(gt_ctx_t *ctx, const char *text, const char *stamp) {
  if(gt_startsWith(text, "found") && isspace((unsigned char)text[5])) {
    gt_parse_found(ctx, text, stamp);
    return;
  }
  /* "found" lines also show up in a normal connect pass; the count the UI
   * reports belongs to the scan the user asked for, not to every inquiry. */
  if(gt_startsWith(text, "ignored") && isspace((unsigned char)text[7])) {
    gt_parse_ignored(ctx, text, stamp);
    return;
  }
  if(gt_startsWith(text, "trying") && isspace((unsigned char)text[6])) {
    gt_parse_trying(ctx, text, stamp);
    return;
  }

  /* --- link state ------------------------------------------------------ *
   * Losing the link always wins; audio, once it flows, is only ever lost and
   * never "downgraded" back to connecting.  The payload prints its progress
   * lines out of order on purpose (it waits for the headset to accept the
   * stream while the local capture is already running), so a plain
   * last-line-wins state machine would flicker. */
  if(gt_matches(text, gt_failed_words)) {
    ctx->link = GT_LINK_FAILED;
    gt_copy_str(ctx->last_error, sizeof(ctx->last_error), text);
  } else if(gt_matches(text, gt_lost_words)) {
    ctx->link = GT_LINK_LOST;
  } else if(gt_matches(text, gt_streaming_words)) {
    ctx->link = GT_LINK_STREAMING;
  } else if(ctx->link != GT_LINK_STREAMING) {
    if(gt_matches(text, gt_scanning_words)) {
      ctx->link = GT_LINK_SEARCHING;
    } else if(gt_matches(text, gt_connecting_words)) {
      ctx->link = GT_LINK_CONNECTING;
    }
  }
  if(gt_startsWith(text, "radio busy") ||
     gt_startsWith(text, "another copy is already running")) {
    ctx->payload_running = 1;
  }

  /* --- per-state details ----------------------------------------------- */
  if(gt_startsWith(text, "connecting")) {
    gt_addr_after(text, "connecting", ctx->payload_addr,
                  sizeof(ctx->payload_addr));
  } else if(gt_startsWith(text, "audio")) {
    ctx->last_error[0] = 0;
    gt_addr_after(text, "audio", ctx->payload_addr,
                  sizeof(ctx->payload_addr));
  }
  if(!strncmp(text, "avdtp: streaming", 16)) {
    const char *p = strstr(text, "SBC");

    if(p) {
      char codec[48];
      size_t o = 0;

      while(p[o] && p[o] != ',' && o + 1 < sizeof(codec)) {
        codec[o] = p[o];
        o++;
      }
      codec[o] = 0;
      gt_copy_str(ctx->codec, sizeof(ctx->codec), gt_trim(codec));
    }
    if((p = strstr(text, "bitpool"))) {
      ctx->bitpool = (int)gt_parse_long(p + 7, 0);
    }
  }
  if(!strncmp(text, "stream:", 7)) {
    ctx->packets = (long)gt_parse_long(text + 7, 0);
    if((strstr(text, "queue"))) {
      const char *q = strstr(text, "queue");

      ctx->queue_ms = (long)gt_parse_long(q + 5, 0);
    }
  }
  if(!strncmp(text, "capture: running", 16) && !ctx->codec[0]) {
    const char *hz = strstr(text, "Hz");

    if(hz) {
      char rate[16];
      size_t o = 0;
      const char *q = hz;

      while(q > text && isdigit((unsigned char)q[-1])) {
        q--;
      }
      while(q < hz && o + 1 < sizeof(rate)) {
        rate[o++] = *q++;
      }
      rate[o] = 0;
      if(rate[0]) {
        snprintf(ctx->codec, sizeof(ctx->codec), "capture %s Hz", rate);
      }
    }
  }

  /* --- configuration echoes -------------------------------------------- */
  /* ghost-toothAPI prints what it read out of headset.ini.  The UI never lets
   * that echo overwrite the file: headset.ini is what the user configured, and
   * the newest echo belongs to the run that is ending, so trusting it would
   * show the previous pick while a new one is already saved.  A value the
   * payload refused is the one case worth surfacing, because then the choice
   * never reaches the radio. */
  if(!strncmp(text, "config: bad address", 19)) {
    char bad[64] = {0};
    const char *p = text + 19;

    gt_read_quoted(&p, bad, sizeof(bad));
    snprintf(ctx->last_error, sizeof(ctx->last_error),
             "headset.ini holds an address the payload refused: %s",
             bad[0] ? bad : "(unreadable)");
  } else if(!strncmp(text, "ghost-toothAPI ", 15) &&
            (isdigit((unsigned char)text[15]) || text[15] == 'v')) {
    /* Only the version banner, never the farewell ("ghost-toothAPI done").  A
     * banner means a run started, so everything the previous run reported
     * becomes history: mixing the last packet count of one attempt with the
     * link state of the next is exactly the kind of lie this UI must not tell. */
    gt_copy_str(ctx->banner, sizeof(ctx->banner), text);
    ctx->codec[0] = 0;
    ctx->payload_addr[0] = 0;
    ctx->last_error[0] = 0;
    ctx->bitpool = 0;
    ctx->packets = 0;
    ctx->queue_ms = 0;
    ctx->link = GT_LINK_SEARCHING;
  } else if(!strncmp(text, "pid:", 4)) {
    ctx->payload_pid = (long)gt_parse_long(text + 4, 0);
  } else if(!strncmp(text, "peer name ", 10)) {
    gt_copy_str(ctx->peer_name, sizeof(ctx->peer_name), gt_trim((char *)text + 10));
  } else if(!strncmp(text, "bond key ", 9)) {
    ctx->bond_present = strstr(text, "saved") ? 1 : 0;
  }

  gt_copy_str(ctx->detail, sizeof(ctx->detail), text);
  gt_refresh_device_flags(ctx);
}

/* ------------------------------------------------------------------ pump */

void
gt_log_pump(gt_ctx_t *ctx) {
  char *buf = ctx->pump_buf;
  long size = 0;
  size_t carry = 0;
  size_t used = 0;
  long cursor = ctx->log_cursor;
  FILE *f;

  pthread_mutex_lock(&ctx->pump_lock);
  pthread_mutex_lock(&ctx->lock);
  if(gt_file_size(ctx->cfg.log_path, &size)) {
    ctx->log_size = 0;
    goto out;
  }
  ctx->log_size = size;
  if(size < cursor) {
    cursor = 0;                    /* truncated log, or the console rebooted */
  }
  if(size == cursor && !ctx->pending[0]) {
    goto out;
  }
  if(!(f = fopen(ctx->cfg.log_path, "rb"))) {
    goto out;
  }
  if(cursor) {
    if(fseek(f, (off_t)cursor, SEEK_SET)) {
      fclose(f);
      goto out;
    }
  }
  if(ctx->pending[0]) {
    carry = strlen(ctx->pending);
    memcpy(buf, ctx->pending, carry);
    ctx->pending[0] = 0;
  }
  for(;;) {
    size_t space = sizeof(ctx->pump_buf) - carry - 1;
    size_t got;
    char *nl;

    if(!space) {
      break;
    }
    got = fread(buf + carry, 1, space, f);
    if(!got) {
      break;
    }
    used = carry + got;
    cursor += (long)got;
    buf[used] = 0;
    while((nl = memchr(buf, '\n', used)) != 0) {
      char stamp[GT_TIME_SIZE] = {0};
      char *line = buf;
      size_t linelen = (size_t)(nl - buf);

      *nl = 0;
      if(linelen >= 9 && line[2] == ':' && line[5] == ':' && line[8] == ' ') {
        memcpy(stamp, line, 8);
        stamp[8] = 0;
        line += 9;
      } else {
        gt_now(stamp, sizeof(stamp));
      }
      line = gt_trim(line);
      if(*line) {
        gt_push_line(ctx, stamp, line);
        gt_apply_log_line(ctx, line, stamp);
        ctx->dirty = 1;
      }
      used -= linelen + 1;
      if(used) {
        memmove(buf, nl + 1, used);
      }
      buf[used] = 0;
    }
    carry = used;
    if(got < space) {
      break;                       /* EOF: keep the remainder for next pump */
    }
  }
  if(carry && carry < sizeof(ctx->pending)) {
    memcpy(ctx->pending, buf, carry);
    ctx->pending[carry] = 0;
  } else {
    ctx->pending[0] = 0;
  }
  ctx->log_cursor = cursor;
  fclose(f);
out:
  pthread_mutex_unlock(&ctx->lock);
  pthread_mutex_unlock(&ctx->pump_lock);
}

void
gt_push_line(gt_ctx_t *ctx, const char *stamp, const char *text) {
  int idx = ctx->line_head;

  ctx->line_head = (ctx->line_head + 1) % GT_MAX_LINES;
  if(ctx->line_count < GT_MAX_LINES) {
    ctx->line_count++;
  }
  gt_copy_str(ctx->line_time[idx], sizeof(ctx->line_time[idx]), stamp);
  gt_copy_str(ctx->lines[idx], sizeof(ctx->lines[idx]), text);
}

/* UI-side notes are written into the same ring as the payload lines and are
 * tagged "ui:" so that nobody reads a message from this payload as a Bluetooth
 * event.  The prefix is added here, never at the call site. */
void
gt_ui_log_add(gt_ctx_t *ctx, const char *fmt, ...) {
  char text[GT_LINE_SIZE];
  char stamp[GT_TIME_SIZE];
  va_list args;

  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  if(strncmp(text, "ui:", 3)) {
    memmove(text + 4, text, sizeof(text) - 4);
    memcpy(text, "ui: ", 4);
    text[sizeof(text) - 1] = 0;
  }
  gt_now(stamp, sizeof(stamp));
  pthread_mutex_lock(&ctx->lock);
  gt_push_line(ctx, stamp, text);
  pthread_mutex_unlock(&ctx->lock);
}

/* ---------------------------------------------------------------- reports */

const char *
gt_link_name(int link) {
  switch(link) {
    case GT_LINK_SEARCHING:  return "scanning";
    case GT_LINK_CONNECTING: return "connecting";
    case GT_LINK_STREAMING:  return "streaming";
    case GT_LINK_FAILED:     return "failed";
    case GT_LINK_LOST:       return "disconnected";
    default:                 return "idle";
  }
}

void
gt_state_json(gt_ctx_t *ctx, gt_jb_t *jb) {
  const char *mode = ctx->mode == GT_MODE_ADDRESS   ? "address"
                     : ctx->mode == GT_MODE_NAME    ? "name"
                                                     : "auto";

  gt_jb_obj_open(jb, "payload");
  gt_jb_bool(jb, "running", ctx->payload_running);
  if(ctx->payload_pid > 0) {
    gt_jb_num(jb, "pid", ctx->payload_pid);
  } else {
    gt_jb_null(jb, "pid");
  }
  gt_jb_str(jb, "elf", ctx->cfg.payload_elf);
  gt_jb_str(jb, "banner", ctx->banner);
  gt_jb_str(jb, "logPath", ctx->cfg.log_path);
  gt_jb_str(jb, "iniPath", ctx->cfg.ini_path);
  gt_jb_bool(jb, "autoStop", ctx->cfg.auto_stop);
  gt_jb_bool(jb, "silentScan", ctx->cfg.scan_silent);
  gt_jb_num(jb, "scanSeconds", ctx->cfg.scan_seconds);
  gt_jb_bool(jb, "loaderReady", ctx->loader_ok == 1);
  gt_jb_obj_close(jb);

  gt_jb_obj_open(jb, "config");
  gt_jb_str(jb, "mode", mode);
  gt_jb_str(jb, "address", ctx->mode == GT_MODE_ADDRESS ? ctx->want_addr : 0);
  gt_jb_str(jb, "name", ctx->mode == GT_MODE_NAME ? ctx->want_name : 0);
  gt_jb_obj_close(jb);

  gt_jb_obj_open(jb, "link");
  gt_jb_str(jb, "state", gt_link_name(ctx->link));
  gt_jb_str(jb, "addr", ctx->payload_addr);
  {
    gt_device_t *d = ctx->payload_addr[0] ? gt_device_find(ctx, ctx->payload_addr)
                                         : 0;

    gt_jb_str(jb, "name", d && d->name[0] ? d->name : ctx->peer_name);
    if(d) {
      gt_jb_str(jb, "kind", gt_kind_name(d->kind));
    }
  }
  gt_jb_str(jb, "codec", ctx->codec);
  gt_jb_num(jb, "bitpool", ctx->bitpool);
  gt_jb_num(jb, "packets", ctx->packets);
  gt_jb_num(jb, "queueMs", ctx->queue_ms);
  gt_jb_str(jb, "detail", ctx->detail);
  gt_jb_str(jb, "error", ctx->last_error);
  gt_jb_obj_close(jb);

  gt_jb_obj_open(jb, "bond");
  gt_jb_bool(jb, "keySaved", ctx->bond_present);
  gt_jb_str(jb, "peerName", ctx->peer_name);
  gt_jb_str(jb, "path", ctx->cfg.bond_path);
  gt_jb_obj_close(jb);

  gt_jb_obj_open(jb, "scan");
  gt_jb_bool(jb, "active", ctx->scan_active);
  gt_jb_bool(jb, "done", ctx->scan_done);
  gt_jb_num(jb, "found", ctx->scan_found);
  gt_jb_str(jb, "startedAt", ctx->scan_started);
  gt_jb_str(jb, "note", ctx->scan_note);
  gt_jb_obj_close(jb);
}

void
gt_devices_json(gt_ctx_t *ctx, gt_jb_t *jb) {
  gt_device_t *order[GT_MAX_DEVICES];
  int n = ctx->device_count;
  int i;
  int j;

  /* newest sighting first; insertion sort because n <= 64 */
  for(i = 0; i < n; i++) {
    order[i] = &ctx->devices[i];
  }
  for(i = 1; i < n; i++) {
    gt_device_t *key = order[i];

    for(j = i - 1; j >= 0 && order[j]->seq < key->seq; j--) {
      order[j + 1] = order[j];
    }
    order[j + 1] = key;
  }

  gt_jb_arr_open(jb, "devices");
  for(i = 0; i < n; i++) {
    gt_device_t *d = order[i];

    gt_jb_obj_open(jb, 0);
    gt_jb_str(jb, "addr", d->addr);
    gt_jb_str(jb, "name", d->name);
    gt_jb_str(jb, "class", d->cls);
    gt_jb_num(jb, "rssi", d->rssi);
    gt_jb_num(jb, "score", d->score);
    gt_jb_bool(jb, "ignored", d->ignored);
    gt_jb_bool(jb, "paired", d->paired);
    gt_jb_bool(jb, "pinned", d->pinned);
    gt_jb_bool(jb, "connected",
               ctx->link == GT_LINK_STREAMING && d->addr[0] &&
               !strcmp(d->addr, ctx->payload_addr));
    gt_jb_str(jb, "kind", gt_kind_name(d->kind));
    gt_jb_str(jb, "seenAt", d->time);
    gt_jb_num(jb, "sightings", d->sightings);
    gt_jb_obj_close(jb);
  }
  gt_jb_arr_close(jb);
}

void
gt_log_json(gt_ctx_t *ctx, gt_jb_t *jb, long total) {
  int n = ctx->line_count;
  int start = (ctx->line_head - n + GT_MAX_LINES) % GT_MAX_LINES;
  int i;

  gt_jb_obj_open(jb, "log");
  gt_jb_num(jb, "bytes", ctx->log_size);
  gt_jb_num(jb, "totalLines", total);
  gt_jb_arr_open(jb, "lines");
  for(i = 0; i < n; i++) {
    int idx = (start + i) % GT_MAX_LINES;

    gt_jb_obj_open(jb, 0);
    gt_jb_str(jb, "t", ctx->line_time[idx]);
    gt_jb_str(jb, "text", ctx->lines[idx]);
    gt_jb_obj_close(jb);
  }
  gt_jb_arr_close(jb);
  gt_jb_obj_close(jb);
}
