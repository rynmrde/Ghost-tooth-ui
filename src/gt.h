/* gt.h - Ghost Tooth UI for PS5
 *
 * Shared types and prototypes for the companion payload that gives
 * ghost-toothAPI a manual, on-console user interface.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <limits.h>
#include <strings.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

/* Every default below can be overridden with -D from the build (the Makefile
 * keeps VERSION and TITLE_ID in one place); guarding them instead of writing the
 * value twice is what stops "GT_VERSION redefined" the moment a release bumps. */
#ifndef GT_NAME
#define GT_NAME        "ghost-tooth-ui"
#endif
#ifndef GT_VERSION
#define GT_VERSION     "0.1.0"
#endif
#ifndef GT_DEFAULT_PORT
#define GT_DEFAULT_PORT 8899
#endif
#ifndef GT_DEFAULT_ROOT
#define GT_DEFAULT_ROOT "/data/ghost-toothAPI"
#endif
#ifdef TITLE_ID                       /* the payload is built with -DTITLE_ID */
#undef  GT_DEFAULT_TITLE_ID
#define GT_DEFAULT_TITLE_ID TITLE_ID
#endif
#ifdef TILE_NAME
#undef  GT_DEFAULT_TILE_NAME
#define GT_DEFAULT_TILE_NAME TILE_NAME
#endif
#ifndef GT_DEFAULT_TITLE_ID
#define GT_DEFAULT_TITLE_ID "GTTH00001"
#endif
#ifndef GT_DEFAULT_TILE_NAME
#define GT_DEFAULT_TILE_NAME "Ghost Tooth"
#endif

#define GT_STR2(x) #x
#define GT_STR(x)  GT_STR2(x)

/* Media tab on the PS5 home screen.  0x10000 is the "Media / streaming
 * app" application category the dashboard filters on; the same value is
 * used by other PS5 web-app tiles (Payload Manager, BFpilot, MkPFS-PS5). */
#define GT_TILE_CATEGORY_MEDIA 65536

#define GT_ADDR_SIZE   18   /* "AA:BB:CC:DD:EE:FF" */
#define GT_NAME_SIZE   96
#define GT_TIME_SIZE   16   /* "HH:MM:SS" */
#define GT_LINE_SIZE   320
#define GT_MAX_LINES   120  /* ring buffer of console log lines kept for the UI */
#define GT_MAX_DEVICES 64

/* A device row as it was reported by ghost-toothAPI's own inquiry pass. */
typedef struct gt_device {
  char addr[GT_ADDR_SIZE];
  char name[GT_NAME_SIZE];
  char cls[10];            /* class-of-device, hex, as logged */
  int  rssi;               /* dBm, 0 when unknown */
  int  score;              /* the payload's own pick score, -1 = none */
  int  ignored;            /* 1 when the payload skipped it (TV etc.) */
  int  paired;             /* 1 when a link key exists for it */
  int  pinned;             /* 1 when headset.ini points at this address */
  int  kind;               /* GT_KIND_* */
  char time[GT_TIME_SIZE];
  long seq;                /* log sequence number of the last sighting */
  int  sightings;
} gt_device_t;

enum {
  GT_KIND_UNKNOWN = 0,
  GT_KIND_HEADPHONES,
  GT_KIND_TV,
  GT_KIND_SPEAKER,
  GT_KIND_PHONE
};

/* headset.ini selection mode. */
enum {
  GT_MODE_AUTO = 0,   /* no pin: the payload picks by score */
  GT_MODE_ADDRESS,    /* address=AA:BB:... */
  GT_MODE_NAME        /* name=substring */
};

/* link state machine, derived from the payload log */
enum {
  GT_LINK_IDLE = 0,
  GT_LINK_SEARCHING,
  GT_LINK_CONNECTING,
  GT_LINK_STREAMING,
  GT_LINK_FAILED,
  GT_LINK_LOST
};

typedef struct gt_config {
  char root[PATH_MAX];            /* where ghost-toothAPI keeps its files */
  char payload_elf[PATH_MAX];     /* "" -> search the usual places */
  char log_path[PATH_MAX];
  char ini_path[PATH_MAX];
  char lock_path[PATH_MAX];
  char bond_path[PATH_MAX];
  char peer_path[PATH_MAX];
  char cache_path[PATH_MAX];
  char pid_path[PATH_MAX];
  char sim_path[PATH_MAX];        /* host-build only: mock payload to launch */
  char loader_host[64];           /* elfldr address used to (re)start payload */
  unsigned short port;
  unsigned short loader_port;
  unsigned scan_seconds;
  int  auto_stop;                 /* kill a running payload before restarting */
  int  scan_silent;               /* pin a dead address while scanning */
  int  tile_enable;               /* register the Media-tab tile on start */
  int  uninstall_tile;            /* remove the tile and exit */
  int  demo;                      /* host build: simulate a console */
  char tile_title[16];
  char tile_name[64];
} gt_config_t;

#define GT_LOG_CHUNK (16 * 1024)

typedef struct gt_ctx {
  gt_config_t cfg;
  pthread_mutex_t lock;
  /* Reading the payload's log means holding a cursor into it, and the HTTP
   * handler, the operation worker and the tile installer all want to do that.
   * pump_lock serialises the reader (its scratch buffer and the cursor are
   * shared state), and it is always taken before ctx->lock. */
  pthread_mutex_t pump_lock;
  char pump_buf[GT_LOG_CHUNK];

  /* device table + console log */
  gt_device_t devices[GT_MAX_DEVICES];
  int device_count;
  char lines[GT_MAX_LINES][GT_LINE_SIZE];
  char line_time[GT_MAX_LINES][GT_TIME_SIZE];
  int line_head;                  /* index of next write */
  int line_count;
  long log_cursor;                /* byte offset already consumed */
  long log_size;

  /* selection persisted in headset.ini */
  int  mode;
  char want_addr[GT_ADDR_SIZE];
  char want_name[GT_NAME_SIZE];

  /* live state of the payload process / BT link */
  int  link;
  int  payload_running;
  long payload_pid;
  char payload_addr[GT_ADDR_SIZE];
  char codec[48];
  int  bitpool;
  long packets;
  long queue_ms;
  char banner[80];
  char detail[GT_LINE_SIZE];
  char last_error[GT_LINE_SIZE];
  int  bond_present;
  char peer_name[GT_NAME_SIZE];

  /* parsing bookkeeping */
  char pending[GT_LINE_SIZE];   /* log tail not terminated by a newline yet */
  long seq;                     /* monotonic sighting counter */
  long scan_seq;                /* seq value when the current scan started */
  pthread_t scan_thread;

  /* scan bookkeeping */
  int  scan_active;
  int  scan_found;
  int  scan_done;
  char scan_note[GT_LINE_SIZE];
  char scan_started[GT_TIME_SIZE];
  long scan_start_cursor;

  /* tile registration result */
  int  tile_installed;
  char tile_error[160];
  unsigned short bound_port;

  /* one-slot operation queue, drained by the worker thread */
  int  op_pending;              /* GT_OP_* or -1 */
  char op_arg[128];
  int  op_busy;
  int  op_kind;
  int  op_status;               /* 0 ok, <0 failed, >0 refused/busy */
  char op_name[24];
  char op_message[GT_LINE_SIZE];
  char op_started[GT_TIME_SIZE];
  pthread_t worker;
  int  worker_run;

  /* payload start control */
  int  loader_ok;                 /* -1 unknown, 0 no loader, 1 reachable */
  int  dirty;                     /* state changed since last push to UI */
} gt_ctx_t;

/* ------------------------------------------------------------------ util */
/* gt_util.c */
int  gt_read_file(const char *path, char *buf, size_t size, size_t *got);
int  gt_write_file_atomic(const char *path, const void *data, size_t size);
int  gt_file_size(const char *path, long *size);
int  gt_path_join(char *dst, size_t size, const char *dir, const char *name);
int  gt_file_exists(const char *path);
void gt_mkdir_p(const char *path);
char *gt_trim(char *s);
int  gt_addr_valid(const char *s);
int  gt_addr_normalize(const char *in, char *out, size_t out_size);
void gt_now(char *out, size_t size);
const char *gt_kind_name(int kind);
int  gt_guess_kind(const char *name, const char *cls);
long gt_parse_long(const char *s, int *ok);
void gt_copy_str(char *dst, size_t dst_size, const char *src);

/* gt_json.c - tiny, allocation-free JSON writer */
typedef struct gt_jb {
  char *buf;
  size_t size;
  size_t off;
  int depth;
  int need_comma;
} gt_jb_t;

void gt_jb_begin(gt_jb_t *jb, char *buf, size_t size);
void gt_jb_obj_open(gt_jb_t *jb, const char *key);
void gt_jb_obj_close(gt_jb_t *jb);
void gt_jb_arr_open(gt_jb_t *jb, const char *key);
void gt_jb_arr_close(gt_jb_t *jb);
void gt_jb_str(gt_jb_t *jb, const char *key, const char *val);
void gt_jb_num(gt_jb_t *jb, const char *key, long long val);
void gt_jb_bool(gt_jb_t *jb, const char *key, int val);
void gt_jb_null(gt_jb_t *jb, const char *key);
void gt_jb_raw(gt_jb_t *jb, const char *key, const char *json);
int  gt_jb_error(gt_jb_t *jb);

/* tiny key/value reader for POST bodies ("addr=AA..&name=bose" or the
 * equivalent JSON object). */
int  gt_kv_get(const char *body, const char *key, char *out, size_t out_size);

/* gt_log.c - ghost-toothAPI log consumption */
gt_device_t *gt_device_find(gt_ctx_t *ctx, const char *addr);
gt_device_t *gt_device_touch(gt_ctx_t *ctx, const char *addr);
void gt_push_line(gt_ctx_t *ctx, const char *stamp, const char *text);
const char *gt_link_name(int link);
void gt_log_json(gt_ctx_t *ctx, gt_jb_t *jb, long total_lines);

/* gt_link.c - the bridge to ghost-toothAPI */
void gt_ctx_init(gt_ctx_t *ctx);
void gt_paths_resolve(gt_ctx_t *ctx);
int  gt_payload_running(gt_ctx_t *ctx);
long gt_payload_pid(gt_ctx_t *ctx);
int  gt_payload_stop(gt_ctx_t *ctx);
int  gt_payload_start(gt_ctx_t *ctx);
int  gt_ini_load(gt_ctx_t *ctx);
int  gt_ini_save(gt_ctx_t *ctx, int mode, const char *value);
/* Advances the log cursor, parses every complete line into the state and pushes
 * it into the ring.  Locks by itself (pump_lock, then lock): callers must not
 * hold either mutex around it. */
void gt_log_pump(gt_ctx_t *ctx);
void gt_cache_save(gt_ctx_t *ctx);
void gt_cache_load(gt_ctx_t *ctx);
enum {
  GT_OP_NONE = -1,
  GT_OP_SCAN = 0,     /* silent inquiry pass, then restore the selection */
  GT_OP_SCAN_LIVE,    /* inquiry pass that lets the payload connect as usual */
  GT_OP_CONNECT,      /* pin an address and restart the payload */
  GT_OP_CONNECT_NAME, /* filter by name and restart the payload */
  GT_OP_AUTO,         /* drop the pin (does not restart) */
  GT_OP_APPLY,        /* restart the payload with the saved selection */
  GT_OP_STOP,         /* stop the payload */
  GT_OP_FORGET,       /* remove the stored link key */
  GT_OP_TILE          /* re-register the Media-tab tile */
};

/* 0 = starts right away, 1 = accepted behind a running operation; <0 only for
 * an argument this build cannot honour.  The queue holds one operation and the
 * newest request replaces a pending one. */
const char *gt_op_human(int kind);
int  gt_op_request(gt_ctx_t *ctx, int kind, const char *arg);
int  gt_link_worker_start(gt_ctx_t *ctx);
void gt_link_worker_stop(gt_ctx_t *ctx);
/* immediate variants, used by the worker and by the tests */
int  gt_op_scan(gt_ctx_t *ctx, int silent);
int  gt_op_connect(gt_ctx_t *ctx, const char *addr);
int  gt_op_connect_name(gt_ctx_t *ctx, const char *name);
int  gt_op_auto(gt_ctx_t *ctx);
int  gt_op_apply(gt_ctx_t *ctx);
int  gt_op_forget(gt_ctx_t *ctx);
void gt_state_refresh(gt_ctx_t *ctx);
void gt_op_json(gt_ctx_t *ctx, gt_jb_t *jb);
void gt_state_json(gt_ctx_t *ctx, gt_jb_t *jb);
void gt_devices_json(gt_ctx_t *ctx, gt_jb_t *jb);
void gt_ui_log_add(gt_ctx_t *ctx, const char *fmt, ...);
gt_ctx_t *gt_ctx_get(void);

/* gt_httpd.c */
int  gt_httpd_bind(unsigned short *port_io);
int  gt_httpd_accept_loop(gt_ctx_t *ctx);
void gt_httpd_stop(void);
void gt_httpd_set_lan(int enabled);

/* gt_tile.c */
int  gt_tile_install(gt_ctx_t *ctx);
int  gt_tile_remove(gt_ctx_t *ctx);

/* gt_notify.c */
void gt_notify(const char *fmt, ...);

/* gt_proc.c */
long gt_proc_find(const char *name);
int  gt_proc_kill(long pid, int sig);
int  gt_proc_priv_escape(void);

/* gt_assets.c */
typedef void (*gt_asset_iter)(const char *path, const uint8_t *data,
                              size_t size, const char *mime, void *arg);
void gt_assets_register_all(void);
const uint8_t *gt_assets_find(const char *path, size_t *size,
                              const char **mime);

/* embedded ghost-toothAPI image (only when built with GT_EMBED_PAYLOAD) */
#ifdef GT_EMBED_PAYLOAD
extern const uint8_t ghost_toothAPI_elf[];
extern const uint8_t ghost_toothAPI_elf_end[];
#define GT_EMBED_PAYLOAD_SIZE ((size_t)(ghost_toothAPI_elf_end - ghost_toothAPI_elf))
#endif

