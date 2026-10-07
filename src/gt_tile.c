/* gt_tile.c - puts the Ghost Tooth icon on the PS5 "Media" page.
 *
 * A Media-tab tile is a plain app directory in /user/app/<TITLEID>/sce_sys with
 * a param.json that carries "applicationCategoryType": 65536 (the media/web-app
 * category the dashboard filters on) and a "deeplinkUri" that the shell hands to
 * its WebKit container.  Registering it is the same flow the other PS5 web-app
 * tiles use (Payload Manager, BFpilot, MkPFS-PS5): stage the metadata and icon,
 * then re-register the title through sceAppInstUtil.
 *
 * Because the launcher URI embeds the port, the tile is refreshed whenever the
 * listener had to move to another port - that is why this runs after bind().
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef GT_PS5
#include <ps5/kernel.h>

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *);

/* NID of sceAppInstUtilAppInstallTitleDir, which registers one staged title
 * directory instead of rescanning everything.  Resolved through the kernel so
 * that no import table entry is required. */
#define GT_NID_APP_INSTALL_TITLE_DIR "Wudg3Xe3heE"
#endif

#define GT_TILE_PARAM_MAX  1024
#define GT_TILE_ATTEMPTS   3

static int
sync_dir(const char *path) {
#ifdef GT_PS5
  char parent[PATH_MAX];
  char *slash;
  int fd;

  if(strlen(path) >= sizeof(parent)) {
    return -1;
  }
  strcpy(parent, path);
  if(!(slash = strrchr(parent, '/')) || slash == parent) {
    return -1;
  }
  *slash = 0;
  if((fd = open(parent, O_RDONLY)) < 0) {
    return -1;
  }
  (void)fsync(fd);
  close(fd);
#endif
  return 0;
}

static int
read_back(const char *path, const void *want, size_t size) {
  struct stat st;
  unsigned char buf[GT_TILE_PARAM_MAX];
  const unsigned char *w = (const unsigned char *)want;
  size_t done = 0;
  int fd;

  if(stat(path, &st) || (size_t)st.st_size != size) {
    return -1;
  }
  if(size > sizeof(buf)) {
    return 0;                       /* too big to verify - assume staged ok */
  }
  if((fd = open(path, O_RDONLY)) < 0) {
    return -1;
  }
  while(done < size) {
    ssize_t n = read(fd, buf + done, size - done);

    if(n <= 0) {
      close(fd);
      return -1;
    }
    done += (size_t)n;
  }
  close(fd);
  return memcmp(buf, w, size) ? -1 : 0;
}

static int
stage_file(const char *path, const void *data, size_t size) {
  if(read_back(path, data, size) == 0) {
    return 0;                        /* already exactly what we want */
  }
  if(gt_write_file_atomic(path, data, size)) {
    return -1;
  }
  if(read_back(path, data, size)) {
    return -1;
  }
  (void)sync_dir(path);
  return 1;
}

static int
build_param_json(const gt_ctx_t *ctx, char *out, size_t size) {
  char uri[128];
  int n;

  snprintf(uri, sizeof(uri), "http://127.0.0.1:%u/",
           (unsigned)(ctx->bound_port ? ctx->bound_port : ctx->cfg.port));
  n = snprintf(out, size,
               "{\n"
               "  \"applicationCategoryType\": %d,\n"
               "  \"titleId\": \"%s\",\n"
               "  \"deeplinkUri\": \"%s\",\n"
               "  \"localizedParameters\": {\n"
               "    \"defaultLanguage\": \"en-US\",\n"
               "    \"en-US\": {\"titleName\": \"%s\"}\n"
               "  }\n"
               "}\n",
               GT_TILE_CATEGORY_MEDIA, ctx->cfg.tile_title, uri,
               ctx->cfg.tile_name);
  return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

#ifdef GT_PS5
static int
install_title_dir(const char *title_id, const char *dir) {
  int (*sceAppInstUtilAppInstallTitleDir)(const char *, const char *, void *);
  uint32_t handle = 0;

  sceAppInstUtilAppInstallTitleDir = 0;
  if(!kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) && handle) {
    sceAppInstUtilAppInstallTitleDir = (void *)kernel_dynlib_resolve(
        -1, handle, GT_NID_APP_INSTALL_TITLE_DIR);
  }
  if(sceAppInstUtilAppInstallTitleDir) {
    return sceAppInstUtilAppInstallTitleDir(title_id, dir, 0);
  }
  return sceAppInstUtilAppInstallAll(0);
}
#endif

int
gt_tile_install(gt_ctx_t *ctx) {
  char base[PATH_MAX];
  char sce_sys[PATH_MAX];
  char param_path[PATH_MAX];
  char icon_path[PATH_MAX];
  char pending_path[PATH_MAX];
  char param_json[GT_TILE_PARAM_MAX];
  const uint8_t *icon;
  size_t icon_size = 0;
  const char *icon_mime;
  long junk = 0;
  int param_rc;
  int icon_rc;
  int pending;
#ifdef GT_PS5
  int err;
#endif

  ctx->tile_error[0] = 0;
  if(build_param_json(ctx, param_json, sizeof(param_json))) {
    gt_copy_str(ctx->tile_error, sizeof(ctx->tile_error), "param.json overflow");
    return -1;
  }
  icon = gt_assets_find("/icon0.png", &icon_size, &icon_mime);

#ifdef GT_PS5
  if(snprintf(base, sizeof(base), "/user/app/%s", ctx->cfg.tile_title) >=
     (int)sizeof(base) ||
     gt_path_join(sce_sys, sizeof(sce_sys), base, "sce_sys") ||
     gt_path_join(param_path, sizeof(param_path), sce_sys, "param.json") ||
     gt_path_join(icon_path, sizeof(icon_path), sce_sys, "icon0.png")) {
    gt_copy_str(ctx->tile_error, sizeof(ctx->tile_error), "title id too long");
    return -1;
  }
  /* Deliberately outside sce_sys: the shell validates that directory's content,
   * and a stray file in there is a good way to make a tile vanish. */
  if(snprintf(pending_path, sizeof(pending_path), "/user/app/.%s-pending",
              ctx->cfg.tile_title) >= (int)sizeof(pending_path)) {
    pending_path[0] = 0;
  }

  if((err = sceAppInstUtilInitialize())) {
    snprintf(ctx->tile_error, sizeof(ctx->tile_error),
             "sceAppInstUtilInitialize: 0x%08X", (unsigned)err);
    return -1;
  }
  gt_mkdir_p(sce_sys);
#else
  /* Host build: stage the identical tree under the payload root so the tile
   * content can be inspected without a console. */
  gt_path_join(base, sizeof(base), ctx->cfg.root, "ui-tile");
  gt_path_join(sce_sys, sizeof(sce_sys), base, "sce_sys");
  gt_path_join(param_path, sizeof(param_path), sce_sys, "param.json");
  gt_path_join(icon_path, sizeof(icon_path), sce_sys, "icon0.png");
  gt_path_join(pending_path, sizeof(pending_path), base, ".pending");
  gt_mkdir_p(sce_sys);
#endif

  param_rc = stage_file(param_path, param_json, strlen(param_json));
  icon_rc = icon && icon_size ? stage_file(icon_path, icon, icon_size) : 0;
  if(param_rc < 0 || icon_rc < 0) {
    char which[96];

    gt_copy_str(which, sizeof(which), param_rc < 0 ? param_path : icon_path);
    snprintf(ctx->tile_error, sizeof(ctx->tile_error),
             "could not stage %s (errno %d)", which, errno);
    return -1;
  }
  /* A crash between the uninstall and the install below would leave the console
   * with no tile at all, and "the staged files are unchanged" would then look
   * like a healthy state forever.  So the refresh is bracketed by a marker file,
   * and an unchanged tree with a marker left behind is refreshed again. */
  pending = pending_path[0] && !gt_file_size(pending_path, &junk);
  if(param_rc <= 0 && icon_rc <= 0 && !pending) {
    ctx->tile_installed = 1;
    gt_ui_log_add(ctx, "Media tile %s already registered", ctx->cfg.tile_title);
    return 0;
  }
  if(pending_path[0]) {
    char note[128];
    int n = snprintf(note, sizeof(note), "pending http://127.0.0.1:%u/\n",
                     (unsigned)ctx->bound_port);

    if(n > 0) {
      (void)gt_write_file_atomic(pending_path, note, (size_t)n);
    }
  }

#ifdef GT_PS5
  {
    int attempt;

    /* Re-registering an existing title is not reliable on every firmware, so
     * only this payload's own title id is removed first, and only after the
     * replacement files above have been written and read back. */
    (void)sceAppInstUtilAppUnInstall(ctx->cfg.tile_title);   /* absent = fine */
    err = -1;
    for(attempt = 0; attempt < GT_TILE_ATTEMPTS; attempt++) {
      err = install_title_dir(ctx->cfg.tile_title, "/user/app/");
      if(!err) {
        break;
      }
      usleep(250 * 1000);
      if(read_back(param_path, param_json, strlen(param_json))) {
        gt_copy_str(ctx->tile_error, sizeof(ctx->tile_error),
                    "param.json disappeared from the title directory");
        return -1;
      }
    }
    if(err) {
      sceAppInstUtilTerminate();
      snprintf(ctx->tile_error, sizeof(ctx->tile_error),
               "AppInstall failed: 0x%08X (will retry on the next start)",
               (unsigned)err);
      return -1;
    }
    /* Nothing else here uses the service, and this payload is long-lived: an
     * initialized AppInst client can hold the installer busy. */
    sceAppInstUtilTerminate();
  }
#endif
  if(pending_path[0]) {
    unlink(pending_path);
  }
  ctx->tile_installed = 1;
  gt_ui_log_add(ctx, "Media tile %s registered (opens http://127.0.0.1:%u/)",
                ctx->cfg.tile_title, (unsigned)ctx->bound_port);
  return 0;
}

int
gt_tile_remove(gt_ctx_t *ctx) {
#ifdef GT_PS5
  int err;

  if((err = sceAppInstUtilInitialize())) {
    snprintf(ctx->tile_error, sizeof(ctx->tile_error),
             "sceAppInstUtilInitialize: 0x%08X", (unsigned)err);
    return -1;
  }
  err = sceAppInstUtilAppUnInstall(ctx->cfg.tile_title);
  sceAppInstUtilTerminate();
  if(err) {
    snprintf(ctx->tile_error, sizeof(ctx->tile_error),
             "sceAppInstUtilAppUnInstall: 0x%08X", (unsigned)err);
    return -1;
  }
#endif
  ctx->tile_installed = 0;
  gt_ui_log_add(ctx, "Media tile %s removed", ctx->cfg.tile_title);
  return 0;
}
