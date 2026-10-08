/* main.c - ghost-tooth-ui: a manual headset picker for ghost-toothAPI on PS5.
 *
 * Startup order matters and is the whole product:
 *
 *   1. resolve the ghost-toothAPI file layout (and publish the embedded ELF
 *      if the console has no copy to launch),
 *   2. bind the HTTP listener, because the Media-tab tile has to carry the
 *      port that actually bound,
 *   3. register/refresh the tile so the UI shows up under Media,
 *   4. prime the UI state from the payload's existing log, so the device
 *      list is not empty on first open,
 *   5. start the operation worker, then serve.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static gt_ctx_t g_ctx;

gt_ctx_t *
gt_ctx_get(void) {
  return &g_ctx;
}

static void
usage(void) {
  puts("ghost-tooth-ui " GT_VERSION " - headset picker for ghost-toothAPI");
  puts("");
  puts("usage: ghost-tooth-ui.elf [options]");
  puts("");
  puts("  --port=N           http port to try first (default " GT_STR(GT_DEFAULT_PORT) ")");
  puts("  --root=PATH        ghost-toothAPI data dir (default " GT_DEFAULT_ROOT ")");
  puts("  --elf=PATH         ghost-toothAPI.elf to (re)start");
  puts("  --loader=HOST:PORT payload loader socket (default 127.0.0.1:9021)");
  puts("  --scan-seconds=N   inquiry window for one scan (default 24)");
  puts("  --no-silent-scan   let the payload connect to its own pick while scanning");
  puts("  --no-autostop      never signal a running payload, only report it");
  puts("  --no-tile          do not register the Media-tab tile");
  puts("  --uninstall-tile   remove the tile and exit");
  puts("  --tile-title=TID   title id the tile is registered under");
  puts("  --tile-name=NAME   dashboard label (default \"Ghost Tooth\")");
  puts("  --lan              also answer on the LAN (default on)");
  puts("  --no-lan           listen on 127.0.0.1 only");
  puts("  --demo             host build: say the console is simulated");
  puts("  --sim=PATH         host build: payload stand-in to launch (dev only)");
  puts("  --version          print the version and exit");
}

static int
parse_int(const char *s, int min, int max, int *out) {
  char *end;
  long v;

  errno = 0;
  v = strtol(s, &end, 10);
  if(errno || end == s || *end || v < min || v > max) {
    return -1;
  }
  *out = (int)v;
  return 0;
}

static void
parse_args(gt_ctx_t *ctx, int argc, char **argv) {
  gt_config_t *cfg = &ctx->cfg;
  int i;

  for(i = 1; i < argc; i++) {
    const char *arg = argv[i];

    if(!strncmp(arg, "--port=", 7)) {
      int v;

      if(parse_int(arg + 7, 1, 65535, &v)) {
        fprintf(stderr, "ignoring invalid --port\n");
      } else {
        cfg->port = (unsigned short)v;
      }
    } else if(!strncmp(arg, "--root=", 7)) {
      gt_copy_str(cfg->root, sizeof(cfg->root), arg + 7);
    } else if(!strncmp(arg, "--elf=", 6)) {
      gt_copy_str(cfg->payload_elf, sizeof(cfg->payload_elf), arg + 6);
    } else if(!strncmp(arg, "--sim=", 6)) {
      gt_copy_str(cfg->sim_path, sizeof(cfg->sim_path), arg + 6);
    } else if(!strncmp(arg, "--loader=", 9)) {
      char *colon = strchr((char *)arg + 9, ':');
      int v;

      if(colon) {
        *colon = 0;
        gt_copy_str(cfg->loader_host, sizeof(cfg->loader_host), arg + 9);
        *colon = ':';
        if(!parse_int(colon + 1, 1, 65535, &v)) {
          cfg->loader_port = (unsigned short)v;
        }
      } else if(!parse_int(arg + 9, 1, 65535, &v)) {
        cfg->loader_port = (unsigned short)v;
      }
    } else if(!strncmp(arg, "--scan-seconds=", 15)) {
      int v;

      if(!parse_int(arg + 15, 5, 300, &v)) {
        cfg->scan_seconds = (unsigned)v;
      }
    } else if(!strncmp(arg, "--tile-title=", 13)) {
      gt_copy_str(cfg->tile_title, sizeof(cfg->tile_title), arg + 13);
    } else if(!strncmp(arg, "--tile-name=", 12)) {
      gt_copy_str(cfg->tile_name, sizeof(cfg->tile_name), arg + 12);
    } else if(!strcmp(arg, "--no-silent-scan")) {
      cfg->scan_silent = 0;
    } else if(!strcmp(arg, "--no-autostop")) {
      cfg->auto_stop = 0;
    } else if(!strcmp(arg, "--no-tile")) {
      cfg->tile_enable = 0;
    } else if(!strcmp(arg, "--no-lan")) {
      gt_httpd_set_lan(0);
    } else if(!strcmp(arg, "--lan")) {
      gt_httpd_set_lan(1);
    } else if(!strcmp(arg, "--uninstall-tile")) {
      cfg->uninstall_tile = 1;
    } else if(!strcmp(arg, "--demo")) {
      cfg->demo = 1;
    } else if(!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
      usage();
      exit(0);
    } else if(!strcmp(arg, "--version")) {
      puts(GT_NAME " " GT_VERSION);
      exit(0);
    } else {
      fprintf(stderr, "unknown option: %s\n", arg);
    }
  }

  /* Environment overrides exist for the host harness and for people who
   * start the payload from a script rather than a loader config. */
  {
    const char *env = getenv("GTT_PORT");
    int v;

    if(env && !parse_int(env, 1, 65535, &v)) {
      cfg->port = (unsigned short)v;
    }
    env = getenv("GTT_ROOT");
    if(env && *env) {
      gt_copy_str(cfg->root, sizeof(cfg->root), env);
    }
  }
}

/* Replays the tail of an existing log so opening the UI shows what the
 * payload has already seen.  The file is opened "ab" by ghost-toothAPI and
 * never truncated, so only the last bytes are worth parsing. */
static void
gt_log_prime(gt_ctx_t *ctx) {
  long size = 0;
  long window = 96 * 1024;

  if(gt_file_size(ctx->cfg.log_path, &size) || size <= 0) {
    return;
  }
  if(size > window) {
    long start = size - window;
    FILE *f = fopen(ctx->cfg.log_path, "rb");
    int c;

    if(!f) {
      return;
    }
    if(fseek(f, (off_t)start, SEEK_SET) == 0) {
      /* step to the end of the partial line we landed in */
      while((c = fgetc(f)) != EOF && c != '\n') {
        start++;
      }
      if(c == EOF) {
        start = size;
      } else {
        start++;
      }
    }
    fclose(f);
    ctx->log_cursor = start;
  }
  gt_log_pump(ctx);
}

int
main(int argc, char **argv) {
  gt_ctx_t *ctx = &g_ctx;
  unsigned short port;

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);          /* the payload we launch is not our child */

  gt_ctx_init(ctx);
  parse_args(ctx, argc, argv);
  gt_paths_resolve(ctx);
  gt_mkdir_p(ctx->cfg.root);
  gt_ini_load(ctx);
  gt_cache_load(ctx);
  gt_log_prime(ctx);

  if(ctx->cfg.uninstall_tile) {
    ctx->bound_port = ctx->cfg.port;
    return gt_tile_remove(ctx) ? 1 : 0;
  }

  port = ctx->cfg.port;
  if(gt_httpd_bind(&port)) {
    fprintf(stderr, "could not listen on port %u: %s\n", (unsigned)ctx->cfg.port,
            strerror(errno));
    gt_notify("could not start the web server (port busy?)");
    return 1;
  }
  ctx->bound_port = port;

  if(ctx->cfg.tile_enable) {
    if(gt_tile_install(ctx)) {
      fprintf(stderr, "Media tile not registered: %s\n", ctx->tile_error);
    }
  }

  if(gt_link_worker_start(ctx)) {
    fprintf(stderr, "operation worker could not start\n");
  }

  gt_ui_log_add(ctx, "ghost-tooth-ui %s listening on port %u, payload root %s",
                GT_VERSION, (unsigned)port, ctx->cfg.root);
  if(ctx->cfg.tile_enable && ctx->tile_installed) {
    gt_notify("Ghost Tooth is on the Media tab.\nOpen it to pick a headset "
              "(port %u).", (unsigned)port);
  } else {
    gt_notify("Ghost Tooth UI ready.\nOpen http://<ps5-ip>:%u/ to pick a "
              "headset.", (unsigned)port);
  }
  if(!ctx->cfg.payload_elf[0]) {
    gt_ui_log_add(ctx, "warning - no ghost-toothAPI.elf found; saving a "
                  "selection still works, starting it does not");
  }

  gt_httpd_accept_loop(ctx);
  gt_link_worker_stop(ctx);
  return 0;
}
