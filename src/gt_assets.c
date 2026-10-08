/* gt_assets.c - the files the UI serves, embedded in the payload.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"
#include "gt_incbin.h"

#include <string.h>

GT_INCASSET(gtt_index_html, "assets/index.html");
GT_INCASSET(gtt_main_css, "assets/main.css");
GT_INCASSET(gtt_main_js, "assets/main.js");
GT_INCASSET(gtt_icon_png, "assets/icon0.png");
#ifdef GT_EMBED_PAYLOAD
GT_INCASSET(ghost_toothAPI_elf, "ghost-toothAPI.elf");
#endif

typedef struct gt_asset {
  const char *path;
  const uint8_t *data;
  size_t size;
  const char *mime;
} gt_asset_t;

static gt_asset_t g_assets[] = {
  {"/index.html", 0, 0, "text/html; charset=utf-8"},
  {"/main.css",   0, 0, "text/css; charset=utf-8"},
  {"/main.js",    0, 0, "application/javascript; charset=utf-8"},
  {"/icon0.png",  0, 0, "image/png"},
};

static int g_ready;

void
gt_assets_register_all(void) {
  if(g_ready) {
    return;
  }
  g_ready = 1;
  g_assets[0].data = gtt_index_html;
  g_assets[0].size = GT_INCASSET_SIZE(gtt_index_html);
  g_assets[1].data = gtt_main_css;
  g_assets[1].size = GT_INCASSET_SIZE(gtt_main_css);
  g_assets[2].data = gtt_main_js;
  g_assets[2].size = GT_INCASSET_SIZE(gtt_main_js);
  g_assets[3].data = gtt_icon_png;
  g_assets[3].size = GT_INCASSET_SIZE(gtt_icon_png);
}

const uint8_t *
gt_assets_find(const char *path, size_t *size, const char **mime) {
  size_t i;

  gt_assets_register_all();
  if(!path) {
    return 0;
  }
  for(i = 0; i < sizeof(g_assets) / sizeof(g_assets[0]); i++) {
    if(!strcmp(path, g_assets[i].path) && g_assets[i].size) {
      if(size) {
        *size = g_assets[i].size;
      }
      if(mime) {
        *mime = g_assets[i].mime;
      }
      return g_assets[i].data;
    }
  }
  return 0;
}
