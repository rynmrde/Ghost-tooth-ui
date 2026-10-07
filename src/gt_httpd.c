/* gt_httpd.c - a deliberately small HTTP/1.1 server for the console.
 *
 * One accept loop, one short-lived thread per connection, no external
 * dependency (the PS5 payload SDK has no libmicrohttpd in the base image),
 * and no request pipelining: the UI is a handful of fetch() calls per second.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define GT_MAX_BODY     (8 * 1024)
#define GT_MAX_HEADER   (16 * 1024)
#define GT_MAX_CLIENTS  6
#define GT_RESP_SIZE    (96 * 1024)

static gt_ctx_t *g_ctx;
static int g_listen_fd = -1;
static volatile int g_stop;
static pthread_mutex_t g_clients_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_clients;
static int g_lan_ok = 1;

void
gt_httpd_set_lan(int enabled) {
  g_lan_ok = enabled ? 1 : 0;
}

/* ------------------------------------------------------------------ helpers */

static const char *
gt_mime_for(const char *path) {
  const char *dot = strrchr(path, '.');

  if(!dot) {
    return "application/octet-stream";
  }
  if(!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
  if(!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
  if(!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
  if(!strcmp(dot, ".json")) return "application/json; charset=utf-8";
  if(!strcmp(dot, ".png"))  return "image/png";
  if(!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
  if(!strcmp(dot, ".svg"))  return "image/svg+xml";
  if(!strcmp(dot, ".ico"))  return "image/x-icon";
  if(!strcmp(dot, ".txt") || !strcmp(dot, ".log")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

static int
send_all(int fd, const char *buf, size_t len) {
  size_t done = 0;

  while(done < len) {
    ssize_t n = write(fd, buf + done, len - done);

    if(n < 0) {
      if(errno == EINTR) {
        continue;
      }
      return -1;
    }
    if(!n) {
      return -1;
    }
    done += (size_t)n;
  }
  return 0;
}

static int
gt_url_decode(char *s) {
  char *d = s;
  size_t i = 0;

  for(i = 0; s[i]; i++) {
    if(s[i] == '%' && s[i + 1] && s[i + 2]) {
      char hex[3] = {s[i + 1], s[i + 2], 0};
      char *end;
      long v = strtol(hex, &end, 16);

      if(*end) {
        return -1;
      }
      if(v == 0) {
        return -1;
      }
      *d++ = (char)v;
      i += 2;
      continue;
    }
    if(s[i] == '+') {
      *d++ = ' ';
      continue;
    }
    if(s[i] < 0x20 || s[i] == 0x7f) {
      return -1;
    }
    *d++ = s[i];
  }
  *d = 0;
  return 0;
}

/* The PS5 web-app container and a phone browser are both cross-origin with
 * respect to nothing here, but a permissive CORS header makes the API usable
 * from a saved bookmarklet or a curl session while debugging. */
static const char *
gt_status_text(int status) {
  switch(status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "OK";
  }
}

static int
gt_respond(int fd, int status, const char *mime, const char *body, size_t len,
           int keep_alive, int head_only) {
  char head[512];
  int n;

  n = snprintf(head, sizeof(head),
               "HTTP/1.1 %d %s\r\n"
               "Content-Type: %s\r\n"
               "Content-Length: %zu\r\n"
               "Cache-Control: no-store\r\n"
               "Access-Control-Allow-Origin: *\r\n"
               "Access-Control-Allow-Headers: Content-Type\r\n"
               "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
               "Connection: %s\r\n"
               "\r\n",
               status, gt_status_text(status),
               mime ? mime : "text/plain; charset=utf-8",
               len, keep_alive ? "keep-alive" : "close");
  if(n < 0 || (size_t)n >= sizeof(head)) {
    return -1;
  }
  if(send_all(fd, head, (size_t)n)) {
    return -1;
  }
  if(head_only || !len) {
    return 0;
  }
  return send_all(fd, body, len);
}

/* ------------------------------------------------------------------- routes */

static int
gt_build_state(gt_ctx_t *ctx, char *buf, size_t size) {
  gt_jb_t jb;

  gt_jb_begin(&jb, buf, size);
  gt_jb_obj_open(&jb, 0);
  gt_jb_str(&jb, "product", GT_NAME);
  gt_jb_str(&jb, "version", GT_VERSION);
  gt_jb_str(&jb, "payload", "ghost-toothAPI");
#ifdef GT_HOST_SIM
  gt_jb_str(&jb, "target", "host");
#else
  gt_jb_str(&jb, "target", "ps5");
#endif
  gt_jb_obj_open(&jb, "tile");
  gt_jb_bool(&jb, "installed", ctx->tile_installed);
  gt_jb_str(&jb, "titleId", ctx->cfg.tile_title);
  gt_jb_str(&jb, "name", ctx->cfg.tile_name);
  gt_jb_str(&jb, "category", "Media");
  gt_jb_num(&jb, "categoryType", GT_TILE_CATEGORY_MEDIA);
  {
    char uri[128];

    snprintf(uri, sizeof(uri), "http://127.0.0.1:%u/",
             (unsigned)ctx->bound_port);
    gt_jb_str(&jb, "deeplinkUri", uri);
  }
  gt_jb_str(&jb, "error", ctx->tile_error[0] ? ctx->tile_error : 0);
  gt_jb_obj_close(&jb);

  gt_jb_obj_open(&jb, "server");
  gt_jb_num(&jb, "port", ctx->bound_port);
  gt_jb_bool(&jb, "lan", g_lan_ok);
  gt_jb_num(&jb, "loaderPort", ctx->cfg.loader_port);
  gt_jb_bool(&jb, "demoMode", ctx->cfg.demo);
  gt_jb_obj_close(&jb);

  gt_jb_obj_open(&jb, "capabilities");
  /* What this build can do, so the UI can grey out what it must not offer:
   * a loader is required to (re)start the payload, and the tile only exists on
   * a console where AppInst answers. */
  gt_jb_bool(&jb, "host", 0);
#ifdef GT_HOST_SIM
  gt_jb_bool(&jb, "host", 1);
#endif
  gt_jb_bool(&jb, "loader", ctx->loader_ok);
  gt_jb_bool(&jb, "silentScan", ctx->cfg.scan_silent);
  gt_jb_bool(&jb, "autoStop", ctx->cfg.auto_stop);
  gt_jb_bool(&jb, "tile", ctx->cfg.tile_enable);
  gt_jb_bool(&jb, "stop", 1);
  gt_jb_bool(&jb, "forget", 1);
  gt_jb_obj_close(&jb);

  gt_state_json(ctx, &jb);
  gt_devices_json(ctx, &jb);
  gt_log_json(ctx, &jb, (long)ctx->line_count);
  gt_op_json(ctx, &jb);
  gt_jb_obj_close(&jb);
  return gt_jb_error(&jb) ? -1 : 0;
}

/* The UI always POSTs {"action":...,"arg":...} to /api/action.  A path form is
 * accepted too, which keeps the API usable from a shell on the desk:
 *   curl -X POST 'http://ps5:8899/api/connect?addr=AA:BB:CC:DD:EE:FF'
 */
static int
gt_action_request(gt_ctx_t *ctx, int fd, const char *path, const char *body,
                  int keep_alive, int head_only, char *buf, size_t size) {
  char action[32] = {0};
  char arg[128] = {0};
  char query[512];
  const char *quest = path ? strchr(path, '?') : 0;
  int rc;
  gt_jb_t jb;

  query[0] = 0;
  if(quest) {
    gt_copy_str(query, sizeof(query), quest + 1);
  }
  gt_kv_get(body, "action", action, sizeof(action));
  if(!action[0]) {
    gt_kv_get(query, "action", action, sizeof(action));
  }
  if(!action[0] && path && !strncmp(path, "/api/", 5)) {
    char seg[64];
    const char *start = path + 5;
    size_t len = quest ? (size_t)(quest - start) : strlen(start);

    if(len >= sizeof(seg)) {
      len = sizeof(seg) - 1;
    }
    memcpy(seg, start, len);
    seg[len] = 0;
    gt_copy_str(action, sizeof(action), seg);
    if(!strcmp(action, "action") || !strcmp(action, "state")) {
      action[0] = 0;
    }
  }

  gt_kv_get(body, "arg", arg, sizeof(arg));
  if(!arg[0]) {
    gt_kv_get(query, "arg", arg, sizeof(arg));
  }
  if(!arg[0]) {
    /* "addr" and "name" are the natural spellings for the two kinds of pick */
    gt_kv_get(body, "addr", arg, sizeof(arg));
    if(!arg[0]) {
      gt_kv_get(query, "addr", arg, sizeof(arg));
    }
    if(!arg[0]) {
      gt_kv_get(body, "name", arg, sizeof(arg));
    }
    if(!arg[0]) {
      gt_kv_get(query, "name", arg, sizeof(arg));
    }
  }
  if(!strcmp(action, "scan")) {
    rc = gt_op_request(ctx, ctx->cfg.scan_silent ? GT_OP_SCAN : GT_OP_SCAN_LIVE, "");
  } else if(!strcmp(action, "scan-live")) {
    rc = gt_op_request(ctx, GT_OP_SCAN_LIVE, "");
  } else if(!strcmp(action, "connect")) {
    if(gt_url_decode(arg)) {
      rc = -1;
    } else if(!arg[0]) {
      rc = -1;
    } else if(strchr(arg, ':')) {
      /* Anything with a colon is meant as an address; refusing it here saves
       * the user a payload restart that could only fail. */
      char addr[GT_ADDR_SIZE];

      rc = gt_addr_normalize(arg, addr, sizeof(addr)) ? -1
                                                       : gt_op_request(ctx, GT_OP_CONNECT, addr);
    } else {
      rc = gt_op_request(ctx, GT_OP_CONNECT, arg);
    }
  } else if(!strcmp(action, "name")) {
    if(gt_url_decode(arg)) {
      rc = -1;
    } else {
      rc = gt_op_request(ctx, GT_OP_CONNECT_NAME, arg);
    }
  } else if(!strcmp(action, "auto")) {
    rc = gt_op_request(ctx, GT_OP_AUTO, "");
  } else if(!strcmp(action, "apply")) {
    rc = gt_op_request(ctx, GT_OP_APPLY, "");
  } else if(!strcmp(action, "stop")) {
    rc = gt_op_request(ctx, GT_OP_STOP, "");
  } else if(!strcmp(action, "forget")) {
    rc = gt_op_request(ctx, GT_OP_FORGET, "");
  } else if(!strcmp(action, "tile")) {
    rc = gt_op_request(ctx, GT_OP_TILE, "");
  } else {
    rc = -2;
  }

  gt_jb_begin(&jb, buf, size);
  gt_jb_obj_open(&jb, 0);
  gt_jb_str(&jb, "action", action);
  if(rc == -2) {
    gt_jb_bool(&jb, "ok", 0);
    gt_jb_str(&jb, "error", "unknown action");
  } else if(rc == 1) {
    /* accepted, but behind whatever the worker is doing right now */
    gt_jb_bool(&jb, "ok", 1);
    gt_jb_bool(&jb, "queued", 1);
    gt_jb_str(&jb, "detail", "queued behind the operation that is running");
  } else if(rc < 0) {
    gt_jb_bool(&jb, "ok", 0);
    gt_jb_str(&jb, "error", "the value was not something ghost-toothAPI accepts");
  } else {
    gt_jb_bool(&jb, "ok", 1);
    gt_jb_bool(&jb, "queued", 0);
    gt_jb_str(&jb, "detail", "started on the ghost-tooth-ui worker");
  }
  gt_jb_obj_close(&jb);
  if(gt_jb_error(&jb)) {
    return gt_respond(fd, 500, "application/json; charset=utf-8",
                      "{\"ok\":false,\"error\":\"response too large\"}", 46,
                      keep_alive, head_only);
  }
  /* A request this build cannot honour answers 4xx so that a script driving the
   * API notices, instead of watching a payload that never restarts. */
  return gt_respond(fd, rc < 0 ? 400 : 200, "application/json; charset=utf-8",
                    buf, strlen(buf), keep_alive, head_only);
}

static void
gt_handle_conn(gt_ctx_t *ctx, int fd) {
  char *req;
  char *body;
  char *line;
  char *save = 0;
  char *eoh;
  char *path;
  char *method;
  char *buf;
  size_t content_length = 0;
  int keep_alive = 0;
  int head_only = 0;

  req = malloc(GT_MAX_HEADER);
  buf = malloc(GT_RESP_SIZE);
  if(!req || !buf) {
    free(req);
    free(buf);
    return;
  }
  {
    size_t got = 0;

    for(;;) {
      ssize_t n;

      if(got + 1 >= GT_MAX_HEADER) {
        goto done;
      }
      n = read(fd, req + got, GT_MAX_HEADER - got - 1);
      if(n <= 0) {
        if(n < 0 && errno == EINTR) {
          continue;
        }
        goto done;
      }
      got += (size_t)n;
      req[got] = 0;
      if((eoh = strstr(req, "\r\n\r\n"))) {
        break;
      }
      if(strstr(req, "\n\n")) {
        eoh = strstr(req, "\n\n");
        break;
      }
    }
    if(!eoh) {
      goto done;
    }
    if(strstr(req, "Connection: keep-alive") || strstr(req, "connection: keep-alive")) {
      keep_alive = 1;
    }
    {
      char *cl = strcasestr(req, "Content-Length:");
      int ok;

      if(cl) {
        content_length = (size_t)gt_parse_long(cl + 15, &ok);
        if(!ok || content_length > GT_MAX_BODY) {
          gt_respond(fd, 413, "text/plain", "body too large", 14, 0, 0);
          goto done;
        }
      }
    }
    /* body starts after the blank line */
    body = eoh + 4;
    {
      size_t have = got - (size_t)(body - req);

      while(have < content_length) {
        ssize_t n = read(fd, body + have, content_length - have);

        if(n <= 0) {
          break;
        }
        have += (size_t)n;
      }
      body[have] = 0;               /* the body ends here for the parsers below */
    }

    line = strtok_r(req, "\r\n", &save);
    if(!line) {
      goto done;
    }
    method = line;
    path = strchr(line, ' ');
    if(!path) {
      /* Not "METHOD PATH VERSION".  Answering beats dropping the socket: a
       * mistyped curl invocation gets told what is wrong instead of hanging. */
      gt_respond(fd, 400, "text/plain; charset=utf-8",
                 "{\"ok\":false,\"error\":\"bad request line\"}", 42, 0, 0);
      goto done;
    }
    *path++ = 0;
    {
      char *sp = strchr(path, ' ');

      if(sp) {
        *sp = 0;
      }
    }
    head_only = !strcmp(method, "HEAD");
    if(strcmp(method, "GET") && strcmp(method, "HEAD") &&
       strcmp(method, "POST") && strcmp(method, "OPTIONS")) {
      gt_respond(fd, 405, "text/plain", "method not allowed", 18, 0, 0);
      goto done;
    }
    if(!strcmp(method, "OPTIONS")) {
      gt_respond(fd, 204, "text/plain", 0, 0, keep_alive, 0);
      goto done;
    }

    if(!strncmp(path, "/api/", 5)) {
      if(!strcmp(path, "/api/health")) {
        gt_respond(fd, 200, "application/json; charset=utf-8",
                   "{\"ok\":true}", 11, keep_alive, head_only);
        goto done;
      }
      if(!strcmp(method, "POST")) {
        gt_action_request(ctx, fd, path, body, keep_alive, head_only, buf,
                          GT_RESP_SIZE);
        goto done;
      }
      /* Reading is refreshing: the payload's log file is the only window into
       * the radio, and this endpoint is the only thing the UI polls, so the
       * pump has to happen here rather than on a timer of its own. */
      gt_state_refresh(ctx);
      if(!strcmp(path, "/api/devices") || !strcmp(path, "/api/log")) {
        gt_jb_t jb;
        int devices = !strcmp(path, "/api/devices");

        gt_jb_begin(&jb, buf, GT_RESP_SIZE);
        gt_jb_obj_open(&jb, 0);
        gt_jb_str(&jb, "product", GT_NAME);
        if(devices) {
          gt_devices_json(ctx, &jb);
        } else {
          gt_log_json(ctx, &jb, (long)ctx->line_count);
        }
        gt_jb_obj_close(&jb);
        if(gt_jb_error(&jb)) {
          gt_respond(fd, 500, "application/json; charset=utf-8",
                     "{\"ok\":false,\"error\":\"response too large\"}", 46,
                     keep_alive, head_only);
          goto done;
        }
        gt_respond(fd, 200, "application/json; charset=utf-8", buf,
                   strlen(buf), keep_alive, head_only);
        goto done;
      }
      if(gt_build_state(ctx, buf, GT_RESP_SIZE)) {
        gt_respond(fd, 500, "application/json; charset=utf-8",
                   "{\"ok\":false,\"error\":\"state too large\"}", 40,
                   keep_alive, head_only);
        goto done;
      }
      gt_respond(fd, 200, "application/json; charset=utf-8", buf, strlen(buf),
                 keep_alive, head_only);
      goto done;
    }

    /* static assets */
    {
      const char *asset_path = !strcmp(path, "/") ? "/index.html" : path;
      const uint8_t *data;
      const char *mime = 0;
      size_t size = 0;

      data = gt_assets_find(asset_path, &size, &mime);
      if(!data) {
        gt_respond(fd, 404, "text/html; charset=utf-8",
                   "<html><body><h1>404</h1></body></html>", 41, keep_alive,
                   head_only);
        goto done;
      }
      gt_respond(fd, 200, mime ? mime : gt_mime_for(asset_path), (const char *)data,
                 size, keep_alive, head_only);
    }
  }

done:
  free(req);
  free(buf);
}

static void *
gt_conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  struct timeval tv = {.tv_sec = 15, .tv_usec = 0};
  int opt = 1;

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  gt_handle_conn(g_ctx, fd);
  close(fd);
  pthread_mutex_lock(&g_clients_lock);
  g_clients--;
  pthread_mutex_unlock(&g_clients_lock);
  return 0;
}

static int
gt_conn_reserve(void) {
  int ok;

  pthread_mutex_lock(&g_clients_lock);
  ok = g_clients < GT_MAX_CLIENTS;
  if(ok) {
    g_clients++;
  }
  pthread_mutex_unlock(&g_clients_lock);
  return ok;
}

int
gt_httpd_bind(unsigned short *port_io) {
  unsigned short first = *port_io;
  unsigned short port = first;

  for(;;) {
    struct sockaddr_in sa;
    int opt = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
      return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = g_lan_ok ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    if(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(fd, 16) == 0) {
      socklen_t len = sizeof(sa);

      if(getsockname(fd, (struct sockaddr *)&sa, &len) == 0) {
        port = ntohs(sa.sin_port);
      }
      *port_io = port;
      g_listen_fd = fd;
      return 0;
    }
    close(fd);
    if(errno != EADDRINUSE) {
      return -1;
    }
    if(port == 65535) {
      return -1;
    }
    port++;
    if(port == first) {
      return -1;
    }
  }
}

int
gt_httpd_accept_loop(gt_ctx_t *ctx) {
  g_ctx = ctx;
  g_stop = 0;

  while(!g_stop) {
    struct sockaddr_in ca;
    socklen_t len = sizeof(ca);
    int fd = accept(g_listen_fd, (struct sockaddr *)&ca, &len);
    pthread_t trd;

    if(fd < 0) {
      if(errno == EINTR) {
        continue;
      }
      break;
    }
    if(!gt_conn_reserve()) {
      gt_respond(fd, 503, "text/plain", "busy", 4, 0, 0);
      close(fd);
      continue;
    }
    if(pthread_create(&trd, 0, gt_conn_thread, (void *)(intptr_t)fd)) {
      gt_respond(fd, 503, "text/plain", "busy", 4, 0, 0);
      close(fd);
      g_clients--;
      continue;
    }
    pthread_detach(trd);
  }
  return 0;
}

void
gt_httpd_stop(void) {
  g_stop = 1;
  if(g_listen_fd >= 0) {
    shutdown(g_listen_fd, SHUT_RDWR);
    close(g_listen_fd);
    g_listen_fd = -1;
  }
}
