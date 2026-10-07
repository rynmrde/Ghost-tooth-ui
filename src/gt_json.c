/* gt_json.c - a tiny JSON writer plus a key/value reader for POST bodies.
 *
 * The API responses are small and fully static, so the writer keeps zero
 * allocations and simply refuses to overflow (a truncated response is
 * flagged to the caller, which then answers 500 instead of garbage).
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GT_JB_INDENT 0

static void
gt_jb_put(gt_jb_t *jb, const char *s, size_t n) {
  if(jb->off + n >= jb->size) {
    jb->off = jb->size;   /* overflow marker */
    return;
  }
  memcpy(jb->buf + jb->off, s, n);
  jb->off += n;
  jb->buf[jb->off] = 0;
}

static void
gt_jb_sep(gt_jb_t *jb) {
  if(jb->need_comma) {
    gt_jb_put(jb, ",", 1);
  }
}

/* A NULL key means "I am an array element": the separator still has to go out,
 * which is the one thing easy to forget here. */
static void
gt_jb_key(gt_jb_t *jb, const char *key) {
  gt_jb_sep(jb);
  if(!key) {
    return;
  }
  gt_jb_put(jb, "\"", 1);
  gt_jb_put(jb, key, strlen(key));
  gt_jb_put(jb, "\":", 2);
  jb->need_comma = 0;
}

void
gt_jb_begin(gt_jb_t *jb, char *buf, size_t size) {
  jb->buf = buf;
  jb->size = size;
  jb->off = 0;
  jb->depth = 0;
  jb->need_comma = 0;
  if(size) {
    buf[0] = 0;
  }
}

int
gt_jb_error(gt_jb_t *jb) {
  return jb->off >= jb->size;
}

void
gt_jb_obj_open(gt_jb_t *jb, const char *key) {
  gt_jb_key(jb, key);
  gt_jb_put(jb, "{", 1);
  jb->need_comma = 0;
  jb->depth++;
}

void
gt_jb_obj_close(gt_jb_t *jb) {
  if(jb->depth > 0) {
    jb->depth--;
  }
  gt_jb_put(jb, "}", 1);
  jb->need_comma = 1;
}

void
gt_jb_arr_open(gt_jb_t *jb, const char *key) {
  gt_jb_key(jb, key);
  gt_jb_put(jb, "[", 1);
  jb->need_comma = 0;
  jb->depth++;
}

void
gt_jb_arr_close(gt_jb_t *jb) {
  if(jb->depth > 0) {
    jb->depth--;
  }
  gt_jb_put(jb, "]", 1);
  jb->need_comma = 1;
}

void
gt_jb_str(gt_jb_t *jb, const char *key, const char *val) {
  static const char hex[] = "0123456789abcdef";
  char out[GT_LINE_SIZE * 2 + 8];
  size_t o = 0;
  const unsigned char *p;

  gt_jb_key(jb, key);
  if(!val) {
    gt_jb_put(jb, "null", 4);
    jb->need_comma = 1;
    return;
  }
  out[o++] = '"';
  for(p = (const unsigned char *)val; *p && o + 7 < sizeof(out); p++) {
    switch(*p) {
      case '"':  out[o++] = '\\'; out[o++] = '"';  break;
      case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
      case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
      case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
      case '\t': out[o++] = '\\'; out[o++] = 't';  break;
      case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
      case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
      default:
        if(*p < 0x20) {
          out[o++] = '\\';
          out[o++] = 'u';
          out[o++] = '0';
          out[o++] = '0';
          out[o++] = hex[(*p) >> 4];
          out[o++] = hex[(*p) & 15];
        } else if(*p < 0x7f) {
          out[o++] = (char)*p;
        } else {
          /* Pass UTF-8 through untouched: every string the UI displays
           * (device names in particular) arrives from the console log. */
          out[o++] = (char)*p;
        }
        break;
    }
  }
  out[o++] = '"';
  gt_jb_put(jb, out, o);
  jb->need_comma = 1;
}

void
gt_jb_num(gt_jb_t *jb, const char *key, long long val) {
  char buf[32];
  int n;

  gt_jb_key(jb, key);
  n = snprintf(buf, sizeof(buf), "%lld", val);
  if(n > 0) {
    gt_jb_put(jb, buf, (size_t)n);
  }
  jb->need_comma = 1;
}

void
gt_jb_bool(gt_jb_t *jb, const char *key, int val) {
  gt_jb_key(jb, key);
  if(val) {
    gt_jb_put(jb, "true", 4);
  } else {
    gt_jb_put(jb, "false", 5);
  }
  jb->need_comma = 1;
}

void
gt_jb_null(gt_jb_t *jb, const char *key) {
  gt_jb_key(jb, key);
  gt_jb_put(jb, "null", 4);
  jb->need_comma = 1;
}

void
gt_jb_raw(gt_jb_t *jb, const char *key, const char *json) {
  gt_jb_key(jb, key);
  if(json && *json) {
    gt_jb_put(jb, json, strlen(json));
  } else {
    gt_jb_put(jb, "null", 4);
  }
  jb->need_comma = 1;
}

/* Accepts either an urlencoded body ("addr=AA:BB..&name=x") or a flat JSON
 * object ({"addr":"AA:BB.."}).  The UI always uses JSON, the urlencoded
 * form exists so that curl and the PS5 browser address bar can drive the
 * API while debugging. */
static int
gt_kv_value_json(const char *body, const char *key, char *out, size_t out_size) {
  char pat[64];
  const char *p;
  size_t o = 0;

  if(snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) {
    return -1;
  }
  if(!(p = strstr(body, pat))) {
    return -1;
  }
  p += strlen(pat);
  while(*p && (*p == ' ' || *p == ':')) {
    p++;
  }
  if(*p != '"') {
    /* number / true / false */
    while(*p && *p != ',' && *p != '}' && o + 1 < out_size) {
      if(!isspace((unsigned char)*p)) {
        out[o++] = *p;
      }
      p++;
    }
    out[o] = 0;
    return o ? 0 : -1;
  }
  p++;
  while(*p && *p != '"' && o + 1 < out_size) {
    if(*p == '\\' && p[1]) {
      p++;
      switch(*p) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        default:  out[o++] = *p;   break;
      }
      p++;
      continue;
    }
    out[o++] = *p++;
  }
  out[o] = 0;
  return 0;
}

static int
gt_kv_value_form(const char *body, const char *key, char *out, size_t out_size) {
  const char *p = body;
  size_t klen = strlen(key);
  size_t o = 0;

  while(p && *p) {
    const char *eq;
    const char *amp;
    size_t vlen;

    if(strncmp(p, key, klen) || p[klen] != '=') {
      if(!(amp = strchr(p, '&'))) {
        return -1;
      }
      p = amp + 1;
      continue;
    }
    if(!(eq = strchr(p + klen, '='))) {
      return -1;
    }
    amp = strchr(eq, '&');
    vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
    for(size_t i = 0; i < vlen && o + 1 < out_size; i++) {
      char c = eq[1 + i];

      if(c == '+') {
        c = ' ';
      } else if(c == '%' && i + 2 < vlen && isxdigit((unsigned char)eq[2 + i]) &&
                isxdigit((unsigned char)eq[3 + i])) {
        char hex[3] = {eq[2 + i], eq[3 + i], 0};
        c = (char)strtol(hex, 0, 16);
        i += 2;
      }
      out[o++] = c;
    }
    out[o] = 0;
    return 0;
  }
  return -1;
}

int
gt_kv_get(const char *body, const char *key, char *out, size_t out_size) {
  if(out_size) {
    out[0] = 0;
  }
  if(!body || !key) {
    return -1;
  }
  if(!gt_kv_value_json(body, key, out, out_size)) {
    return 0;
  }
  return gt_kv_value_form(body, key, out, out_size);
}
