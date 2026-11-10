/** @file
  PpttBuild: add cache structures to a PPTT that has none (docs/WIN11-USB.md section 10).
  Portable C, no firmware dependencies: used by Star2LtePpttDxe and by the host test.

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#ifndef PPTT_BUILD_H_
#define PPTT_BUILD_H_

#ifdef PPTT_EDK2
#include <Base.h>
typedef UINT8   pb_u8;
typedef UINT16  pb_u16;
typedef UINT32  pb_u32;
#else
#include <stdint.h>
typedef uint8_t   pb_u8;
typedef uint16_t  pb_u16;
typedef uint32_t  pb_u32;
#endif

#define PB_DATA         0u
#define PB_INSTRUCTION  1u
#define PB_UNIFIED      2u
#define PB_MAX_CACHES   8u

//
// One cache as the CPU reports it (CLIDR + CCSIDR), for the cores of the boot CPU's cluster.
// Levels up to PrivateLevels are private to each core; the levels above are shared by the
// cluster. The S9's A55 cluster: PrivateLevels 1 (CLIDR 0x82000023: L1 I+D, one unified L2 of
// 512 KiB, no L3; the L2 is the cluster's shared cache).
//
typedef struct {
  pb_u8     Level;        // 1..7
  pb_u8     Kind;         // PB_DATA, PB_INSTRUCTION, PB_UNIFIED
  pb_u16    LineSize;     // bytes
  pb_u32    Sets;
  pb_u32    Ways;
} PB_CACHE;

//
// Build into Out (capacity OutCap) a copy of the PPTT In with cache structures for every leaf
// processor in the cluster that holds ACPI processor ID 0 (the boot CPU, an A55 here): each such
// core gets its own caches of levels 1..PrivateLevels, referenced from the leaf (level 1) and
// chained through "next level of cache"; the higher levels exist once, listed as the cluster
// node's private resources and reached from the last per-core level. Other nodes are copied
// unchanged (parent offsets remapped). Returns the new length, or 0 with *Why set when the
// input isn't a plain processor-only PPTT this understands (then nothing should change).
//
pb_u32
PpttAddCaches (
  const pb_u8     *In,
  pb_u32          InLen,
  const PB_CACHE  *Caches,
  pb_u32          NumCaches,
  pb_u32          PrivateLevels,
  pb_u8           *Out,
  pb_u32          OutCap,
  const char      **Why
  );

#endif
