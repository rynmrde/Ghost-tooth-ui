/* gt_notify.c - the little banner the console shows when the payload starts.
 *
 * The same mechanism ghost-toothAPI itself uses: a notification request
 * through the kernel, which the shell renders as a system message.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef GT_PS5
typedef struct gt_notify_request {
  char unused[45];
  char message[3075];
} gt_notify_request_t;

int sceKernelSendNotificationRequest(int device, gt_notify_request_t *request,
                                      size_t size, int sync);
#endif

void
gt_notify(const char *fmt, ...) {
  char text[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);

#ifdef GT_PS5
  {
    gt_notify_request_t req;

    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "[%s] %s", GT_NAME, text);
    if(sceKernelSendNotificationRequest(0, &req, sizeof(req), 0) < 0) {
      fprintf(stderr, "notification could not be delivered: %s\n", text);
    }
  }
#else
  printf("[notify] %s\n", text);
#endif
}
