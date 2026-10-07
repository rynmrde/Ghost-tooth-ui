/* gt_link.c - the bridge between the UI and ghost-toothAPI.
 *
 * ghost-toothAPI.elf is a self-contained payload.  It reads its headset pick
 * from /data/ghost-toothAPI/headset.ini, narrates everything it does to
 * /data/ghost-toothAPI/ghost-toothAPI.log, and serialises itself with an
 * exclusive flock() on /data/ghost-toothAPI/ghost-toothAPI.lock.  It exposes no
 * socket, no IPC and no command line, so those three files *are* its control
 * surface and this module is the only place that touches them:
 *
 *   scan     -> optional silent pin, restart payload, tail the log
 *   connect  -> "address=.." in headset.ini, restart payload
 *   name     -> "name=.."   in headset.ini, restart payload
 *   auto     -> no pin in headset.ini
 *   forget   -> remove bond.key + peer.name
 *   status   -> flock probe + log tail
 *
 * Every operation runs on one worker thread so that an HTTP request never
 * blocks for the seconds a Bluetooth inquiry pass takes; the UI polls
 * /api/state and watches the operation slot instead.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define GT_STOP_WAIT_MS   60
#define GT_STOP_WAIT_MAX 1200
#define GT_SCAN_TICK_MS  200
#define GT_HOST_PID_NAME  "ghost-toothAPI.elf"

/* Address no device can ever answer; a silent inquiry pass pins this so the
 * payload scans, logs every device in range, and connects to nothing. */
#define GT_SILENT_ADDR "02:00:00:00:00:00"

static const char gt_ini_header[] =
    "; ghost-toothAPI - optional headset pick\n"
    ";\n"
    "; Leave this file as-is. Headphones are picked automatically\n"
    "; (TVs and random speakers are skipped).\n"
    ";\n"
    "; To force a headset, type part of its name:\n"
    "; name=beats\n"
    ";\n"
    "; Or pick it in the Ghost Tooth UI (Media tab), which writes\n"
    "; an address= line below.\n";

/* ------------------------------------------------------------------ setup */

void
gt_ctx_init(gt_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  pthread_mutex_init(&ctx->lock, 0);
  pthread_mutex_init(&ctx->pump_lock, 0);
  gt_copy_str(ctx->cfg.root, sizeof(ctx->cfg.root), GT_DEFAULT_ROOT);
  ctx->cfg.port = GT_DEFAULT_PORT;
  ctx->cfg.scan_seconds = 24;
  ctx->cfg.auto_stop = 1;
  ctx->cfg.scan_silent = 1;
  ctx->cfg.tile_enable = 1;
  ctx->cfg.loader_port = 9021;
  gt_copy_str(ctx->cfg.loader_host, sizeof(ctx->cfg.loader_host), "127.0.0.1");
  gt_copy_str(ctx->cfg.tile_title, sizeof(ctx->cfg.tile_title), GT_DEFAULT_TITLE_ID);
  gt_copy_str(ctx->cfg.tile_name, sizeof(ctx->cfg.tile_name), GT_DEFAULT_TILE_NAME);
  ctx->loader_ok = -1;
  ctx->mode = GT_MODE_AUTO;
  ctx->link = GT_LINK_IDLE;
  ctx->op_pending = GT_OP_NONE;
  ctx->op_status = 0;
  ctx->payload_pid = -1;
}

static int
gt_probe_payload_elf(const char *root, char *out, size_t out_size) {
  /* ghost-toothAPI.elf has to be a file the loader can read; these are the
   * places people keep it. */
  static const char *const dirs[] = {
    "",                 /* the payload root first */
    "/user/homebrew",
    "/mnt/usb0",
    "/data",
    0
  };
  size_t i;

  for(i = 0; dirs[i]; i++) {
    char path[PATH_MAX];
    struct stat st;

    if(*dirs[i]) {
      gt_path_join(path, sizeof(path), dirs[i], "ghost-toothAPI.elf");
    } else {
      gt_path_join(path, sizeof(path), root, "ghost-toothAPI.elf");
    }
    if(!stat(path, &st) && S_ISREG(st.st_mode) && st.st_size > 4096) {
      gt_copy_str(out, out_size, path);
      return 0;
    }
  }
  return -1;
}

void
gt_paths_resolve(gt_ctx_t *ctx) {
  gt_config_t *cfg = &ctx->cfg;

  gt_path_join(cfg->log_path, sizeof(cfg->log_path), cfg->root, "ghost-toothAPI.log");
  gt_path_join(cfg->ini_path, sizeof(cfg->ini_path), cfg->root, "headset.ini");
  gt_path_join(cfg->lock_path, sizeof(cfg->lock_path), cfg->root, "ghost-toothAPI.lock");
  gt_path_join(cfg->bond_path, sizeof(cfg->bond_path), cfg->root, "bond.key");
  gt_path_join(cfg->peer_path, sizeof(cfg->peer_path), cfg->root, "peer.name");
  gt_path_join(cfg->cache_path, sizeof(cfg->cache_path), cfg->root, "ui-devices.txt");
  gt_path_join(cfg->pid_path, sizeof(cfg->pid_path), cfg->root, "ghost-toothAPI.pid");

  if(!cfg->payload_elf[0]) {
    if(!gt_probe_payload_elf(cfg->root, cfg->payload_elf, sizeof(cfg->payload_elf))) {
      return;
    }
#ifdef GT_HOST_SIM
    /* Host build: the mock payload is the thing we launch, not an ELF. */
    gt_copy_str(cfg->payload_elf, sizeof(cfg->payload_elf), "host-sim");
    return;
#endif
#ifdef GT_EMBED_PAYLOAD
    /* Nothing on the console to launch: publish the image that was embedded at
     * build time so the loader has a file to read.  Only written when absent,
     * never over a running payload. */
    gt_path_join(cfg->payload_elf, sizeof(cfg->payload_elf), cfg->root,
                 "ghost-toothAPI.elf");
    if(!gt_file_exists(cfg->payload_elf) && !gt_payload_running(ctx)) {
      gt_mkdir_p(cfg->root);
      if(!gt_write_file_atomic(cfg->payload_elf, ghost_toothAPI_elf,
                               GT_EMBED_PAYLOAD_SIZE)) {
        gt_ui_log_add(ctx, "published ghost-toothAPI.elf to %s",
                      cfg->payload_elf);
      } else {
        cfg->payload_elf[0] = 0;
      }
    } else if(gt_file_exists(cfg->payload_elf)) {
      return;                       /* keep whatever is already installed */
    }
#endif
    cfg->payload_elf[0] = 0;
  }
}

/* -------------------------------------------------------------------- ini */

/* Length of the key in "key = value", ignoring the padding, so that only a
 * real "address="/"name=" line is treated as a selection. */
static size_t
gt_key_len(const char *start, const char *eq) {
  while(eq > start && isspace((unsigned char)eq[-1])) {
    eq--;
  }
  return (size_t)(eq - start);
}

int
gt_ini_load(gt_ctx_t *ctx) {
  char buf[4096];
  size_t got = 0;
  int rc = gt_read_file(ctx->cfg.ini_path, buf, sizeof(buf), &got);
  char *line;
  char *save = 0;

  ctx->mode = GT_MODE_AUTO;
  ctx->want_addr[0] = 0;
  ctx->want_name[0] = 0;
  if(rc) {
    return rc == 1 ? 0 : -1;        /* a missing ini is the normal case */
  }
  for(line = strtok_r(buf, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
    char *p = gt_trim(line);
    char *value;

    if(*p == ';' || *p == '#' || !*p) {
      continue;                     /* comments never select a headset */
    }
    if(!(value = strchr(p, '='))) {
      continue;
    }
    *value++ = 0;
    p = gt_trim(p);
    value = gt_trim(value);
    if(!strcasecmp(p, "address")) {
      char addr[GT_ADDR_SIZE];

      if(!gt_addr_normalize(value, addr, sizeof(addr))) {
        ctx->mode = GT_MODE_ADDRESS;
        gt_copy_str(ctx->want_addr, sizeof(ctx->want_addr), addr);
      }
    } else if(!strcasecmp(p, "name") && *value) {
      ctx->mode = GT_MODE_NAME;
      gt_copy_str(ctx->want_name, sizeof(ctx->want_name), value);
    }
  }
  return 0;
}

/* Rewrites headset.ini keeping the user's comments: active name=/address=
 * lines are dropped and the new selection is appended.  The payload reads
 * this file once at startup, so a torn file would be a silent misselection;
 * gt_write_file_atomic() gives us temp + fsync + rename instead. */
int
gt_ini_save(gt_ctx_t *ctx, int mode, const char *value) {
  char in[4096];
  char out[6144];
  size_t got = 0;
  size_t o = 0;
  char *line;
  char *save = 0;
  char setting[192];
  char addr[GT_ADDR_SIZE];
  int have_input;

  setting[0] = 0;
  if(mode == GT_MODE_ADDRESS) {
    if(!value || gt_addr_normalize(value, addr, sizeof(addr))) {
      return -1;
    }
    snprintf(setting, sizeof(setting), "address=%s", addr);
  } else if(mode == GT_MODE_NAME) {
    int i;

    if(!value || !*value || strlen(value) > 64) {
      return -1;
    }
    /* headset.ini is parsed line-wise by the payload: refuse anything that
     * could break out of one key/value line or inject a second setting. */
    for(i = 0; value[i]; i++) {
      unsigned char c = (unsigned char)value[i];

      if(c < 0x20 || c == '\n' || c == '=' || c == ';') {
        return -1;
      }
    }
    snprintf(setting, sizeof(setting), "name=%s", value);
  }

  have_input = !gt_read_file(ctx->cfg.ini_path, in, sizeof(in), &got);
  if(!have_input) {
    o = (size_t)snprintf(out, sizeof(out), "%s", gt_ini_header);
    if(setting[0]) {
      o += (size_t)snprintf(out + o, sizeof(out) - o, "\n%s\n", setting);
    }
    if(gt_write_file_atomic(ctx->cfg.ini_path, out, o)) {
      return -1;
    }
    goto done;
  }

  if(got && in[got - 1] != '\n') {
    in[got++] = '\n';
    in[got] = 0;
  }
  for(line = strtok_r(in, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
    char *p = gt_trim(line);
    char *eq;

    if(*p != ';' && *p != '#' &&
       ((eq = strchr(p, '=')) != 0) &&
       (((!strncasecmp(p, "address", 7) && gt_key_len(p, eq) == 7)) ||
        ((!strncasecmp(p, "name", 4) && gt_key_len(p, eq) == 4)))) {
      continue;                     /* drop the previous selection */
    }
    if(o + strlen(line) + 2 >= sizeof(out)) {
      break;
    }
    o += (size_t)snprintf(out + o, sizeof(out) - o, "%s\n", line);
  }
  if(setting[0]) {
    if(o + strlen(setting) + 2 >= sizeof(out)) {
      return -1;
    }
    o += (size_t)snprintf(out + o, sizeof(out) - o, "%s\n", setting);
  }
  if(gt_write_file_atomic(ctx->cfg.ini_path, out, o)) {
    return -1;
  }

done:
  gt_ini_load(ctx);
  return 0;
}

/* ----------------------------------------------------------- device cache */
/* A tab-delimited mirror of the device table, so the list survives a restart
 * of the UI payload.  The payload's log stays the source of truth. */

void
gt_cache_save(gt_ctx_t *ctx) {
  char out[GT_MAX_DEVICES * 224];
  size_t o = 0;
  int i;

  for(i = 0; i < ctx->device_count && o + 200 < sizeof(out); i++) {
    gt_device_t *d = &ctx->devices[i];

    o += (size_t)snprintf(out + o, sizeof(out) - o,
                          "%s\t%s\t%s\t%d\t%d\t%d\t%s\n", d->addr,
                          d->cls[0] ? d->cls : "-", d->name, d->rssi,
                          d->ignored, d->score, d->time);
  }
  (void)gt_write_file_atomic(ctx->cfg.cache_path, out, o);
}

void
gt_cache_load(gt_ctx_t *ctx) {
  char buf[24576];
  size_t got = 0;
  char *line;
  char *save = 0;

  if(gt_read_file(ctx->cfg.cache_path, buf, sizeof(buf), &got)) {
    return;
  }
  for(line = strtok_r(buf, "\n", &save); line; line = strtok_r(0, "\n", &save)) {
    char addr[64] = {0};
    char cls[16] = {0};
    char name[GT_NAME_SIZE] = {0};
    char stamp[GT_TIME_SIZE] = {0};
    long rssi = 0;
    long ignored = 0;
    long score = -1;
    gt_device_t *d;
    char *fields[7];
    char *cur;
    char *last = 0;
    int n = 0;

    for(cur = strtok_r(line, "\t", &last); cur && n < 7; cur = strtok_r(0, "\t", &last)) {
      fields[n++] = cur;
    }
    if(n < 7) {
      continue;
    }
    gt_copy_str(addr, sizeof(addr), fields[0]);
    gt_copy_str(cls, sizeof(cls), fields[1]);
    gt_copy_str(name, sizeof(name), fields[2]);
    rssi = gt_parse_long(fields[3], 0);
    ignored = gt_parse_long(fields[4], 0);
    score = gt_parse_long(fields[5], 0);
    gt_copy_str(stamp, sizeof(stamp), fields[6]);
    if(!gt_addr_valid(addr)) {
      continue;
    }
    d = gt_device_touch(ctx, addr);
    if(strcmp(cls, "-")) {
      gt_copy_str(d->cls, sizeof(d->cls), cls);
    }
    gt_copy_str(d->name, sizeof(d->name), name);
    gt_copy_str(d->time, sizeof(d->time), stamp);
    d->rssi = (int)rssi;
    d->ignored = (int)ignored;
    d->score = (int)score;
    d->kind = gt_guess_kind(name, strcmp(cls, "-") ? cls : "");
    if(d->ignored && d->kind == GT_KIND_UNKNOWN) {
      d->kind = GT_KIND_TV;
    }
  }
}

/* --------------------------------------------------------- payload control */

/* ghost-toothAPI holds an exclusive flock on its lock file for as long as it
 * runs, so a non-blocking attempt from us fails exactly while it is alive.
 * That is a far more reliable liveness signal than a pid lookup. */
int
gt_payload_running(gt_ctx_t *ctx) {
  int running = 0;
  int fd;

  if(!gt_file_exists(ctx->cfg.lock_path)) {
    ctx->payload_running = 0;
    return 0;
  }
  if((fd = open(ctx->cfg.lock_path, O_RDWR)) < 0) {
    ctx->payload_running = 0;
    return 0;
  }
  if(flock(fd, LOCK_EX | LOCK_NB) != 0) {
    running = 1;                    /* EWOULDBLOCK: somebody holds it */
  } else {
    flock(fd, LOCK_UN);
  }
  close(fd);
  ctx->payload_running = running;
  return running;
}

long
gt_payload_pid(gt_ctx_t *ctx) {
  char buf[32];
  size_t got = 0;

  /* Host builds (and the demo harness) leave a pid file behind; on the console
   * the process table is the only source, so look the payload up by the name
   * the loader stamps into its thread name. */
  if(!gt_read_file(ctx->cfg.pid_path, buf, sizeof(buf), &got)) {
    long pid = gt_parse_long(buf, 0);

    if(pid > 0) {
      return pid;
    }
  }
  return gt_proc_find(GT_HOST_PID_NAME);
}

int
gt_payload_stop(gt_ctx_t *ctx) {
  long pid;
  int waited = 0;

  if(!gt_payload_running(ctx)) {
    unlink(ctx->cfg.pid_path);
    return 0;
  }
  if((pid = gt_payload_pid(ctx)) <= 0) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "ghost-toothAPI is running but its pid could not be found - "
                "turn the headset off, it stops itself");
    return -1;
  }
  if(gt_proc_kill(pid, SIGTERM)) {
    gt_proc_priv_escape();
    if(gt_proc_kill(pid, SIGTERM)) {
      gt_copy_str(ctx->detail, sizeof(ctx->detail),
                  "the running payload could not be signalled (it may be "
                  "protected) - turn the headset off to stop it");
      return -1;
    }
  }
  while(waited < GT_STOP_WAIT_MAX) {
    usleep(GT_STOP_WAIT_MS * 1000);
    waited += GT_STOP_WAIT_MS;
    if(!gt_payload_running(ctx)) {
      goto released;
    }
  }
  gt_proc_kill(pid, SIGKILL);
  waited = 0;
  while(waited < GT_STOP_WAIT_MAX) {
    usleep(GT_STOP_WAIT_MS * 1000);
    waited += GT_STOP_WAIT_MS;
    if(!gt_payload_running(ctx)) {
      goto released;
    }
  }
  gt_copy_str(ctx->detail, sizeof(ctx->detail),
              "ghost-toothAPI is still holding the radio lock");
  return -1;

released:
  unlink(ctx->cfg.pid_path);
  ctx->payload_pid = -1;
  return 0;
}

#ifndef GT_HOST_SIM
/* Asks the payload loader that is already listening on the console
 * (ps5-payload-dev/elfldr, tcp/9021) to spawn the file we just configured.
 * One URI line is the whole protocol, exactly like
 * `echo file:/data/ghost-toothAPI/ghost-toothAPI.elf | nc PS5 9021`. */
static int
gt_loader_spawn(const gt_config_t *cfg, const char *elf) {
  struct sockaddr_in sa;
  struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
  char uri[PATH_MAX + 64];
  char reply[256];
  ssize_t wrote;
  ssize_t n;
  int opt = 1;
  int fd;

  if(snprintf(uri, sizeof(uri), "file:%s?pipe=0\n", elf) >= (int)sizeof(uri)) {
    return -1;
  }
  if((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(cfg->loader_port);
  if(inet_pton(AF_INET, cfg->loader_host, &sa.sin_addr) != 1) {
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  }
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  if(connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
    close(fd);
    return -1;
  }
  wrote = write(fd, uri, strlen(uri));
  if(wrote != (ssize_t)strlen(uri)) {
    close(fd);
    return -1;
  }
  shutdown(fd, SHUT_WR);
  n = read(fd, reply, sizeof(reply) - 1);
  close(fd);
  if(n > 0) {
    reply[n] = 0;
    /* elfldr answers with "[elfldr.elf] Error ..." only when it refused. */
    if(strstr(reply, "rror")) {
      return -1;
    }
  }
  return 0;
}

#endif /* !GT_HOST_SIM */

#ifdef GT_HOST_SIM
static int
gt_host_sim_start(const gt_config_t *cfg) {
  char root_arg[PATH_MAX + 16];
  pid_t pid;

  snprintf(root_arg, sizeof(root_arg), "--root=%s", cfg->root);
  fflush(stdout);
  pid = fork();
  if(pid < 0) {
    return -1;
  }
  if(pid) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%d\n", (int)pid);

    (void)gt_write_file_atomic(cfg->pid_path, buf, (size_t)n);
    return 0;
  }
  setsid();
  if(cfg->sim_path[0]) {
    execlp("python3", "python3", cfg->sim_path, root_arg, (char *)0);
  }
  _exit(127);
}
#endif

/* Starts the payload with the selection that is currently in headset.ini.
 * Returns 0 once the loader accepted the image, -1 when the payload could
 * not be started (the message in ctx->detail tells the UI why). */
int
gt_payload_start(gt_ctx_t *ctx) {
  gt_config_t *cfg = &ctx->cfg;

  if(gt_payload_running(ctx)) {
    if(!cfg->auto_stop) {
      gt_copy_str(ctx->detail, sizeof(ctx->detail),
                  "ghost-toothAPI is already running and auto-stop is off - "
                  "stop it in your loader or turn the headset off");
      return -1;
    }
    if(gt_payload_stop(ctx)) {
      return -1;
    }
  }

#ifdef GT_HOST_SIM
  if(gt_host_sim_start(cfg)) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail), "could not start the mock payload");
    return -1;
  }
  ctx->loader_ok = 1;
#else
  if(!cfg->payload_elf[0]) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "ghost-toothAPI.elf was not found - keep it in the payload "
                "root, /user/homebrew, or build with EMBED_PAYLOAD=1");
    return -1;
  }
  if(gt_loader_spawn(cfg, cfg->payload_elf)) {
    ctx->loader_ok = 0;
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "no payload loader answered on port - the selection is saved, "
                "start ghost-toothAPI.elf from your loader to use it");
    return -1;
  }
  ctx->loader_ok = 1;
#endif
  ctx->payload_pid = -1;
  return 0;
}

/* --------------------------------------------------------------- operations */

int
gt_op_scan(gt_ctx_t *ctx, int silent) {
  int saved_mode = ctx->mode;
  char saved_addr[GT_ADDR_SIZE];
  char saved_name[GT_NAME_SIZE];
  long limit;
  int waited = 0;
  int moved = 0;
  long seq_before = ctx->seq;

  gt_copy_str(saved_addr, sizeof(saved_addr), ctx->want_addr);
  gt_copy_str(saved_name, sizeof(saved_name), ctx->want_name);
  limit = (long)ctx->cfg.scan_seconds * 1000;
  if(limit < 5000) {
    limit = 5000;
  }

  ctx->link = GT_LINK_SEARCHING;
  ctx->scan_active = 1;
  ctx->scan_done = 0;
  gt_now(ctx->scan_started, sizeof(ctx->scan_started));
  ctx->scan_found = 0;
  ctx->scan_note[0] = 0;
  /* The error from an earlier attempt is expected to still be on screen; a scan
   * of its own always "fails" (the silent pass pages a dead address), so
   * clearing it here keeps a red warning from surviving every scan afterwards. */
  ctx->last_error[0] = 0;

  if(silent && gt_ini_save(ctx, GT_MODE_ADDRESS, GT_SILENT_ADDR)) {
    gt_copy_str(ctx->scan_note, sizeof(ctx->scan_note),
                "could not write the silent-scan pin to headset.ini");
    return -1;
  }
  if(gt_payload_start(ctx)) {
    if(silent) {
      if(saved_mode == GT_MODE_ADDRESS) {
        gt_ini_save(ctx, saved_mode, saved_addr);
      } else if(saved_mode == GT_MODE_NAME) {
        gt_ini_save(ctx, saved_mode, saved_name);
      } else {
        gt_ini_save(ctx, GT_MODE_AUTO, 0);
      }
    }
    return -1;
  }

  /* The payload logs one "found ..." line per device during a ~15 s inquiry.
   * Poll until it moves on to picking, gives up, or the window closes.
   * Liveness is re-checked every tick: the UI is the one that started the
   * payload, and the scan is over as soon as it exits (silent scan) or gets
   * further than the inquiry (scan-live). */
  while(waited < (int)limit) {
    usleep(GT_SCAN_TICK_MS * 1000);
    waited += GT_SCAN_TICK_MS;
    gt_log_pump(ctx);
    ctx->payload_running = gt_payload_running(ctx);
    if(ctx->seq != seq_before) {
      moved = 1;
      seq_before = ctx->seq;
    }
    if(ctx->scan_found && (ctx->link != GT_LINK_SEARCHING || !ctx->payload_running)) {
      break;
    }
    if(!ctx->payload_running && waited > 3000 && !moved) {
      break;             /* nothing was ever logged: the loader refused it */
    }
  }
  gt_log_pump(ctx);
  ctx->payload_running = gt_payload_running(ctx);

  if(silent) {
    /* Put the user's own selection back and take the radio off the bogus
     * address again, so a scan never leaves the console misconfigured. */
    if(saved_mode == GT_MODE_ADDRESS) {
      gt_ini_save(ctx, saved_mode, saved_addr);
    } else if(saved_mode == GT_MODE_NAME) {
      gt_ini_save(ctx, saved_mode, saved_name);
    } else {
      gt_ini_save(ctx, GT_MODE_AUTO, 0);
    }
    if(ctx->payload_running) {
      gt_payload_stop(ctx);
    }
    ctx->link = GT_LINK_IDLE;
  }
  ctx->scan_active = 0;
  snprintf(ctx->scan_note, sizeof(ctx->scan_note),
           "%d device line(s) collected in %.1f s%s", ctx->scan_found,
           waited / 1000.0,
           silent ? " - silent pass, nothing was connected"
                  : " - the payload may have connected to its own pick");
  gt_cache_save(ctx);
  return 0;
}

int
gt_op_connect(gt_ctx_t *ctx, const char *addr) {
  char norm[GT_ADDR_SIZE];

  if(!addr || gt_addr_normalize(addr, norm, sizeof(norm))) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "that is not a Bluetooth address (AA:BB:CC:DD:EE:FF)");
    return -1;
  }
  if(gt_ini_save(ctx, GT_MODE_ADDRESS, norm)) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "headset.ini could not be written - check the payload root");
    return -1;
  }
  ctx->link = GT_LINK_CONNECTING;
  gt_copy_str(ctx->payload_addr, sizeof(ctx->payload_addr), norm);
  gt_copy_str(ctx->detail, sizeof(ctx->detail),
              "address pinned, starting ghost-toothAPI");
  return gt_payload_start(ctx);
}

int
gt_op_connect_name(gt_ctx_t *ctx, const char *name) {
  if(gt_ini_save(ctx, GT_MODE_NAME, name)) {
    gt_copy_str(ctx->detail, sizeof(ctx->detail),
                "the name filter was rejected (1-64 printable characters)");
    return -1;
  }
  ctx->link = GT_LINK_SEARCHING;
  gt_copy_str(ctx->detail, sizeof(ctx->detail),
              "name filter saved, starting ghost-toothAPI");
  return gt_payload_start(ctx);
}

int
gt_op_auto(gt_ctx_t *ctx) {
  if(gt_ini_save(ctx, GT_MODE_AUTO, 0)) {
    return -1;
  }
  gt_copy_str(ctx->detail, sizeof(ctx->detail),
              "automatic selection restored - the payload will pick the "
              "highest scoring headset on its next start");
  return 0;
}

int
gt_op_apply(gt_ctx_t *ctx) {
  return gt_payload_start(ctx);
}

int
gt_op_forget(gt_ctx_t *ctx) {
  int rc = 0;

  if(unlink(ctx->cfg.bond_path) && errno != ENOENT) {
    rc = -1;
  }
  if(unlink(ctx->cfg.peer_path) && errno != ENOENT) {
    rc = -1;
  }
  ctx->bond_present = 0;
  ctx->peer_name[0] = 0;
  gt_copy_str(ctx->detail, sizeof(ctx->detail),
              rc ? "the stored link key could not be removed"
                 : "stored key removed - the headset will pair from scratch");
  return rc;
}

/* ---------------------------------------------------------- op queue/thread */

/* Short machine names, used in the log lines the UI writes. */
static const char *
gt_op_label(int kind) {
  switch(kind) {
    case GT_OP_SCAN:         return "scan";
    case GT_OP_SCAN_LIVE:    return "scan-live";
    case GT_OP_CONNECT:      return "connect";
    case GT_OP_CONNECT_NAME: return "name";
    case GT_OP_AUTO:         return "auto";
    case GT_OP_APPLY:        return "apply";
    case GT_OP_STOP:         return "stop";
    case GT_OP_FORGET:       return "forget";
    case GT_OP_TILE:         return "tile";
    default:                 return "none";
  }
}

/* What the operator reads in the "working" line. */
const char *
gt_op_human(int kind) {
  switch(kind) {
    case GT_OP_SCAN:         return "scanning for headsets";
    case GT_OP_SCAN_LIVE:    return "scanning, payload picks too";
    case GT_OP_CONNECT:      return "connecting";
    case GT_OP_CONNECT_NAME: return "connecting by name";
    case GT_OP_AUTO:         return "letting the payload pick";
    case GT_OP_APPLY:        return "restarting ghost-toothAPI";
    case GT_OP_STOP:         return "stopping ghost-toothAPI";
    case GT_OP_FORGET:       return "forgetting the pairing";
    case GT_OP_TILE:         return "registering the Media tile";
    default:                 return "idle";
  }
}

int
gt_op_request(gt_ctx_t *ctx, int kind, const char *arg) {
  int queued;

  /* One slot, and the newest click wins it: an operation that has not started
   * yet only ever encodes an intention the user has now changed (a different
   * headset in the list, or "stop" while a restart is still waiting).  Losing
   * that click to "busy, try again" would be the worse failure. */
  pthread_mutex_lock(&ctx->lock);
  queued = ctx->op_busy || ctx->op_pending != GT_OP_NONE;
  ctx->op_pending = kind;
  gt_copy_str(ctx->op_arg, sizeof(ctx->op_arg), arg ? arg : "");
  gt_now(ctx->op_started, sizeof(ctx->op_started));
  pthread_mutex_unlock(&ctx->lock);
  return queued;
}

static void
gt_op_run(gt_ctx_t *ctx, int kind, const char *arg) {
  int rc;

  gt_ui_log_add(ctx, "%s requested", gt_op_human(kind));
  switch(kind) {
    case GT_OP_SCAN:         rc = gt_op_scan(ctx, 1); break;
    case GT_OP_SCAN_LIVE:    rc = gt_op_scan(ctx, 0); break;
    case GT_OP_CONNECT:      rc = gt_op_connect(ctx, arg); break;
    case GT_OP_CONNECT_NAME: rc = gt_op_connect_name(ctx, arg); break;
    case GT_OP_AUTO:         rc = gt_op_auto(ctx); break;
    case GT_OP_APPLY:        rc = gt_op_apply(ctx); break;
    case GT_OP_STOP:         rc = gt_payload_stop(ctx);
                             if(!rc) {
                               ctx->link = GT_LINK_IDLE;
                               gt_copy_str(ctx->detail, sizeof(ctx->detail),
                                           "ghost-toothAPI stopped");
                             }
                             break;
    case GT_OP_FORGET:       rc = gt_op_forget(ctx); break;
    case GT_OP_TILE:         rc = gt_tile_install(ctx); break;
    default:                 rc = -1; break;
  }
  pthread_mutex_lock(&ctx->lock);
  ctx->op_status = rc ? -1 : 0;
  gt_copy_str(ctx->op_message, sizeof(ctx->op_message), ctx->detail);
  pthread_mutex_unlock(&ctx->lock);
  /* gt_ui_log_add() locks by itself, so it must never be called while the
   * mutex is held - doing that stops the worker for good and every later
   * request starts answering with a stale snapshot. */
  gt_ui_log_add(ctx, "%s %s", gt_op_human(kind), rc ? "failed - see the log" : "done");
}

static void *
gt_worker_main(void *arg) {
  gt_ctx_t *ctx = (gt_ctx_t *)arg;

  for(;;) {
    int kind;
    char arg_buf[sizeof(ctx->op_arg)];

    pthread_mutex_lock(&ctx->lock);
    while(ctx->worker_run && ctx->op_pending == GT_OP_NONE) {
      pthread_mutex_unlock(&ctx->lock);
      usleep(150 * 1000);
      pthread_mutex_lock(&ctx->lock);
    }
    if(!ctx->worker_run) {
      pthread_mutex_unlock(&ctx->lock);
      break;
    }
    kind = ctx->op_pending;
    ctx->op_pending = GT_OP_NONE;
    ctx->op_busy = 1;
    ctx->op_kind = kind;
    gt_copy_str(arg_buf, sizeof(arg_buf), ctx->op_arg);
    gt_copy_str(ctx->op_name, sizeof(ctx->op_name), gt_op_human(kind));
    gt_copy_str(ctx->op_message, sizeof(ctx->op_message), "running");
    pthread_mutex_unlock(&ctx->lock);

    gt_op_run(ctx, kind, arg_buf);

    pthread_mutex_lock(&ctx->lock);
    if(kind == GT_OP_SCAN || kind == GT_OP_SCAN_LIVE) {
      ctx->scan_active = 0;
      ctx->scan_done = 1;
    }
    ctx->op_busy = 0;
    pthread_mutex_unlock(&ctx->lock);
  }
  return 0;
}

int
gt_link_worker_start(gt_ctx_t *ctx) {
  pthread_mutex_lock(&ctx->lock);
  if(ctx->worker_run) {
    pthread_mutex_unlock(&ctx->lock);
    return 0;
  }
  ctx->worker_run = 1;
  pthread_mutex_unlock(&ctx->lock);
  if(pthread_create(&ctx->worker, 0, gt_worker_main, ctx)) {
    ctx->worker_run = 0;
    return -1;
  }
  return 0;
}

void
gt_link_worker_stop(gt_ctx_t *ctx) {
  pthread_mutex_lock(&ctx->lock);
  ctx->worker_run = 0;
  ctx->op_pending = GT_OP_NONE;
  pthread_mutex_unlock(&ctx->lock);
}

void
gt_state_refresh(gt_ctx_t *ctx) {
  long size = 0;

  gt_log_pump(ctx);                 /* locks by itself, so: before our lock */
  pthread_mutex_lock(&ctx->lock);
  /* headset.ini can be edited by hand or by another tool, and the payload only
   * reads it at start; re-reading it here keeps the "Selection" the UI shows in
   * step with what the next start will actually do. */
  (void)gt_ini_load(ctx);
  ctx->payload_running = gt_payload_running(ctx);
  if(!ctx->payload_running && ctx->link != GT_LINK_IDLE &&
     ctx->link != GT_LINK_FAILED && ctx->link != GT_LINK_LOST &&
     !ctx->op_busy) {
    ctx->link = GT_LINK_IDLE;
  }
  ctx->payload_pid = ctx->payload_running ? gt_payload_pid(ctx) : -1;
  ctx->bond_present = !gt_file_size(ctx->cfg.bond_path, &size);
  if(ctx->bond_present) {
    char peer[GT_NAME_SIZE];

    if(!gt_read_file(ctx->cfg.peer_path, peer, sizeof(peer), 0)) {
      gt_copy_str(ctx->peer_name, sizeof(ctx->peer_name), gt_trim(peer));
    } else {
      ctx->peer_name[0] = 0;
    }
  } else {
    ctx->peer_name[0] = 0;
  }
  pthread_mutex_unlock(&ctx->lock);
}

void
gt_op_json(gt_ctx_t *ctx, gt_jb_t *jb) {
  gt_jb_obj_open(jb, "op");
  gt_jb_str(jb, "name", ctx->op_name);
  /* the action word the UI sent, so a caller can wait for the op it asked for
   * instead of trusting a human-readable sentence that may be reworded */
  gt_jb_str(jb, "kind", ctx->op_busy || ctx->op_pending != GT_OP_NONE ||
                        ctx->op_kind != GT_OP_NONE ? gt_op_label(ctx->op_kind) : "");
  gt_jb_bool(jb, "busy", ctx->op_busy);
  gt_jb_bool(jb, "queued", ctx->op_pending != GT_OP_NONE);
  gt_jb_num(jb, "status", ctx->op_status);
  gt_jb_str(jb, "message", ctx->op_message);
  gt_jb_str(jb, "startedAt", ctx->op_started);
  gt_jb_obj_close(jb);
}
