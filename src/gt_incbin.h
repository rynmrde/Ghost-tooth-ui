/* gt_incbin.h - embed a build-tree file into .rodata without a code generator.
 *
 * The UI payload ships its web assets (and optionally ghost-toothAPI.elf)
 * inside the ELF, so that one file is all you have to load onto the console.
 * The path is resolved by the assembler relative to the build directory,
 * which is the repo root, so it must be written as "assets/foo".
 *
 * The size is taken from the two symbols and not baked into the data
 * section, which keeps this portable between prospero-clang and gcc/clang
 * on the host.
 *
 * Copyright (C) 2026 Ghost-tooth-ui contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define GT_INCASSET(name, file)             \
  __asm__(".section .rodata\n"              \
          ".global " #name "\n"             \
          ".global " #name "_end\n"         \
          ".align 16\n"                     \
          #name ":\n"                       \
          ".incbin \"" file "\"\n"          \
          #name "_end:\n"                   \
          ".previous\n");                   \
  extern const uint8_t name[];               \
  extern const uint8_t name##_end[];

#define GT_INCASSET_SIZE(name) ((size_t)(name##_end - name))
