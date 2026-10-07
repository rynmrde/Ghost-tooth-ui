/* gt_proc.c - find and signal the running ghost-toothAPI process.
 *
 * ghost-toothAPI keeps exactly one instance alive by taking an exclusive
 * flock() on /data/ghost-toothAPI/ghost-toothAPI.lock, so "is it running" is
 * answered by the lock (gt_link.c).  "which pid" is answered here through the
 * process table, because the payload never writes a pid file: the loader stamps
 * the ELF name into the thread name, which is what we match on.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#ifdef GT_PS5
#include <ps5/kernel.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

/* Matches the loader-assigned process/thread name.  ki_comm is only 16 bytes
 * wide, so the same name is compared as a prefix there and in full in the
 * 20-byte thread name. */
#ifdef GT_PS5
static int
gt_name_match(const char *field, size_t field_size, const char *name) {
  return !strncmp(field, name, field_size) && strlen(name) <= field_size;
}
#endif

long
gt_proc_find(const char *name) {
#ifdef GT_PS5
  struct kinfo_proc *kp;
  size_t len = 0;
  long found = -1;
  int mib[4];
  int count;
  int i;
  void *buf;

  if(!name || !*name) {
    return -1;
  }
  mib[0] = CTL_KERN;
  mib[1] = KERN_PROC;
  mib[2] = KERN_PROC_PROC;
  mib[3] = 0;
  if(sysctl(mib, 4, 0, &len, 0, 0) || !len) {
    return -1;
  }
  if(!(buf = malloc(len))) {
    return -1;
  }
  if(sysctl(mib, 4, buf, &len, 0, 0)) {
    free(buf);
    return -1;
  }
  if(len % sizeof(struct kinfo_proc)) {
    /* The layout does not match this firmware's kinfo_proc; refusing to
     * guess is better than killing an unrelated process. */
    free(buf);
    errno = EPROTO;
    return -1;
  }
  kp = (struct kinfo_proc *)buf;
  count = (int)(len / sizeof(*kp));
  for(i = 0; i < count; i++) {
    if(kp[i].ki_structsize != (int)sizeof(struct kinfo_proc)) {
      continue;
    }
    if(kp[i].ki_pid == getpid()) {
      continue;
    }
    if(gt_name_match(kp[i].ki_tdname, sizeof(kp[i].ki_tdname), name) ||
       gt_name_match(kp[i].ki_comm, sizeof(kp[i].ki_comm), name)) {
      found = (long)kp[i].ki_pid;
      break;
    }
  }
  free(buf);
  return found;
#else
  /* The host build is a development harness: the mock payload leaves a pid
   * file behind, which gt_payload_pid() reads before it ever gets here. */
  (void)name;
  return -1;
#endif
}

int
gt_proc_kill(long pid, int sig) {
  if(pid <= 0) {
    return -1;
  }
  return kill((pid_t)pid, sig);
}

/* A payload started through elfldr normally already runs with the credentials
 * of the process it was injected into, so this is only a fallback for the
 * firmwares where signalling another process comes back EPERM.  Both calls are
 * best effort: without kernel access they simply fail and the caller reports
 * the underlying problem to the UI. */
int
gt_proc_priv_escape(void) {
#ifdef GT_PS5
  if(kernel_get_ucred_uid(0) == 0) {
    return 0;
  }
  if(kernel_set_ucred_uid(0, 0)) {
    return -1;
  }
  kernel_set_ucred_ruid(0, 0);
  kernel_set_ucred_svuid(0, 0);
  kernel_set_ucred_rgid(0, 0);
  return kernel_get_ucred_uid(0) == 0 ? 0 : -1;
#else
  return getpid() == 0 ? 0 : -1;
#endif
}
