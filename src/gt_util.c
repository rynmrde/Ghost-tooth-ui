/* gt_util.c - small filesystem / string helpers shared by every module.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

void
gt_copy_str(char *dst, size_t dst_size, const char *src) {
  size_t n;

  if(!dst || !dst_size) {
    return;
  }
  if(!src) {
    dst[0] = 0;
    return;
  }
  n = strlen(src);
  if(n >= dst_size) {
    n = dst_size - 1;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

int
gt_read_file(const char *path, char *buf, size_t size, size_t *got) {
  FILE *f;
  size_t n;

  if(got) {
    *got = 0;
  }
  if(!path || !buf || size < 1) {
    return -1;
  }
  if(!(f = fopen(path, "rb"))) {
    return errno == ENOENT ? 1 : -1;
  }
  n = fread(buf, 1, size - 1, f);
  fclose(f);
  buf[n] = 0;
  if(got) {
    *got = n;
  }
  return 0;
}

/* Writes leniently: every file the UI touches is read back by
 * ghost-toothAPI, so a half-written file must never be observable.
 * temp + fsync + rename keeps the payload from reading a torn ini. */
int
gt_write_file_atomic(const char *path, const void *data, size_t size) {
  char tmp[PATH_MAX];
  const uint8_t *p = (const uint8_t *)data;
  struct stat st;
  size_t done = 0;
  int error = 0;
  int fd;

  if(snprintf(tmp, sizeof(tmp), "%s.ui-tmp", path) >= (int)sizeof(tmp)) {
    return -1;
  }
  if(lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
    return -1;
  }
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0) {
    return -1;
  }
  while(done < size) {
    ssize_t wrote = write(fd, p + done, size - done);

    if(wrote < 0) {
      if(errno == EINTR) {
        continue;
      }
      error = errno ? errno : EIO;
      break;
    }
    done += (size_t)wrote;
  }
  if(!error && fsync(fd)) {
    error = errno ? errno : EIO;
  }
  if(close(fd) && !error) {
    error = errno ? errno : EIO;
  }
  if(error) {
    unlink(tmp);
    errno = error;
    return -1;
  }
  if(rename(tmp, path)) {
    error = errno ? errno : EIO;
    unlink(tmp);
    errno = error;
    return -1;
  }
  return 0;
}

/* Builds "<dir>/<name>" and fails closed when it would not fit, which keeps
 * -Wformat-truncation honest instead of silenced: a truncated path would make
 * the UI read the wrong file. */
int
gt_path_join(char *dst, size_t size, const char *dir, const char *name) {
  int n;

  if(!dst || !size) {
    return -1;
  }
  dst[0] = 0;
  if(!dir || !name) {
    return -1;
  }
  n = snprintf(dst, size, "%s/%s", dir, name);
  if(n < 0 || (size_t)n >= size) {
    dst[0] = 0;
    return -1;
  }
  return 0;
}

int
gt_file_size(const char *path, long *size) {
  struct stat st;

  if(size) {
    *size = -1;
  }
  if(!path || stat(path, &st)) {
    return -1;
  }
  if(size) {
    *size = (long)st.st_size;
  }
  return 0;
}

int
gt_file_exists(const char *path) {
  struct stat st;

  return path && !stat(path, &st);
}

void
gt_mkdir_p(const char *path) {
  char buf[PATH_MAX];
  size_t i;

  if(!path || !*path) {
    return;
  }
  gt_copy_str(buf, sizeof(buf), path);
  for(i = 1; buf[i]; i++) {
    if(buf[i] == '/') {
      buf[i] = 0;
      if(mkdir(buf, 0755) && errno != EEXIST) {
        return;
      }
      buf[i] = '/';
    }
  }
  mkdir(buf, 0755);
}

char *
gt_trim(char *s) {
  char *end;

  if(!s) {
    return s;
  }
  while(*s && isspace((unsigned char)*s)) {
    s++;
  }
  end = s + strlen(s);
  while(end > s && isspace((unsigned char)end[-1])) {
    end--;
  }
  *end = 0;
  return s;
}

/* ghost-toothAPI only accepts the canonical colon form, and it prints
 * addresses the same way, so the UI normalises to that shape everywhere. */
int
gt_addr_valid(const char *s) {
  int i;

  if(!s || strlen(s) != 17) {
    return 0;
  }
  /* "AA:BB:CC:DD:EE:FF" - hex pairs, colons at index 2, 5, 8, 11, 14 */
  for(i = 0; i < 17; i++) {
    if(i % 3 == 2) {
      if(s[i] != ':') {
        return 0;
      }
    } else if(!isxdigit((unsigned char)s[i])) {
      return 0;
    }
  }
  return 1;
}

int
gt_addr_normalize(const char *in, char *out, size_t out_size) {
  unsigned int b[6];
  int nibbles = 0;
  int i;
  char clean[32];
  size_t n = 0;

  if(!in || out_size < GT_ADDR_SIZE) {
    return -1;
  }
  for(i = 0; in[i] && n < sizeof(clean) - 1; i++) {
    if(in[i] == ':' || in[i] == '-' || in[i] == ' ') {
      continue;
    }
    if(!isxdigit((unsigned char)in[i])) {
      return -1;
    }
    clean[n++] = (char)toupper((unsigned char)in[i]);
    nibbles++;
  }
  clean[n] = 0;
  if(nibbles != 12) {
    return -1;
  }
  for(i = 0; i < 6; i++) {
    char byte[3] = {clean[i * 2], clean[i * 2 + 1], 0};

    b[i] = (unsigned int)strtoul(byte, 0, 16);
  }
  if(snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X", b[0], b[1], b[2],
              b[3], b[4], b[5]) != 17) {
    return -1;
  }
  return 0;
}

void
gt_now(char *out, size_t size) {
  struct timeval tv;
  struct tm tm;
  time_t now;

  if(!out || !size) {
    return;
  }
  now = time(0);
  gettimeofday(&tv, 0);
  if(!gmtime_r(&now, &tm)) {
    gt_copy_str(out, size, "00:00:00");
    return;
  }
  if(strftime(out, size, "%H:%M:%S", &tm) <= 0) {
    gt_copy_str(out, size, "00:00:00");
    return;
  }
}

const char *
gt_kind_name(int kind) {
  switch(kind) {
    case GT_KIND_HEADPHONES: return "headphones";
    case GT_KIND_TV:         return "tv";
    case GT_KIND_SPEAKER:    return "speaker";
    case GT_KIND_PHONE:      return "phone";
    default:                 return "unknown";
  }
}

/* Keyword table mirroring the vendor/product hints ghost-toothAPI itself
 * embeds, so the UI labels a row the same way the payload would score it.
 * It is only a label: the payload remains the thing that picks. */
/* Name keyword tables for the device "kind" hint shown in the UI.  This is only
 * a label: whether ghost-toothAPI will actually skip a device is decided by the
 * payload itself and reported by the "[ignored]" suffix of its "found" line,
 * which gt_log.c turns into gt_device_t::ignored.  Order below matters - people
 * put brand names in front of the product word ("Bose TV Speaker"), so the
 * things a headset owner does not want are matched first. */
static const char *const gt_speaker_words[] = {
  "soundbar", "sound bar", "speaker", "boombox", "car audio", "head unit",
  "stereo system", "hi-fi", "amplifier", "av receiver", 0
};

static const char *const gt_tv_words[] = {
  "tv", "television", "hisense", "bravia", "qled", "oled", "neo", "uhd",
  "webos", "tizen", "vizio", "loewe", "samsung smart", "lg evo", "the frame",
  "monitor", "projector", "receiver", 0
};

static const char *const gt_hp_words[] = {
  "headset", "headphone", "earbud", "earpod", "buds", "over-ear", "on-ear",
  "wh-1000", "wh1000", "wf-1000", "qc35", "qc45", "qc ear", "noise cancelling",
  "soundcore", "jabra", "sennheiser", "bose", "marshall", "edifier",
  "beyerdynamic", "akg", "skullcandy", "arctis", "steelseries", "hyperx",
  "airpods", "galaxy bud", "pixel bud", "playstation portal", "puls", "sony",
  "beats", "studio", "true wireless", "auracast", "gpro", "cloud iii",
  "astra", "vibro", 0
};

static const char *const gt_phone_words[] = {
  "phone", "smartphone", "iphone", "galaxy s", "galaxy note", "galaxy a",
  "pixel 7", "pixel 8", "pixel 9", "oneplus", "xiaomi", 0
};

static int
gt_has_word(const char *lower, const char *const *words) {
  int i;

  for(i = 0; words[i]; i++) {
    if(strstr(lower, words[i])) {
      return 1;
    }
  }
  return 0;
}

int
gt_guess_kind(const char *name, const char *cls) {
  char lower[GT_NAME_SIZE];
  size_t i;

  for(i = 0; name && i < sizeof(lower) - 1; i++) {
    lower[i] = (char)tolower((unsigned char)name[i]);
    if(!name[i]) {
      break;
    }
  }
  lower[i] = 0;

  if(gt_has_word(lower, gt_speaker_words)) {
    return GT_KIND_SPEAKER;
  }
  if(gt_has_word(lower, gt_tv_words)) {
    return GT_KIND_TV;
  }
  if(gt_has_word(lower, gt_hp_words)) {
    return GT_KIND_HEADPHONES;
  }
  if(gt_has_word(lower, gt_phone_words)) {
    return GT_KIND_PHONE;
  }
  /* Nothing matched by name.  The class-of-device field (24 bits, printed as
   * six hex digits by the payload) still says what kind of device the vendor
   * registered: bits 8-12 are the major device class, where 0x04 is
   * "Audio/Video", and bits 2-7 are its minor class. */
  if(cls && *cls) {
    unsigned long cod = strtoul(cls, 0, 16);
    unsigned int major = (unsigned int)((cod >> 8) & 0x1f);
    unsigned int minor = (unsigned int)((cod >> 2) & 0x3f);

    if(major == 0x04) {
      if(minor == 0x08 || minor == 0x09 || minor == 0x1f) {
        return GT_KIND_HEADPHONES;     /* headphones, mic, wearable headset */
      }
      return GT_KIND_SPEAKER;          /* loudspeaker, portable audio, car */
    }
  }
  return GT_KIND_UNKNOWN;
}

long
gt_parse_long(const char *s, int *ok) {
  char *end;
  long v;

  if(ok) {
    *ok = 0;
  }
  if(!s) {
    return 0;
  }
  errno = 0;
  v = strtol(s, &end, 10);
  if(end == s || errno) {
    return 0;
  }
  if(ok) {
    *ok = 1;
  }
  return v;
}
