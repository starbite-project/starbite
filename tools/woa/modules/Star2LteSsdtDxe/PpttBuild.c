/** @file
  PpttBuild: add cache structures to a PPTT that has none (PpttBuild.h).

  ACPI 6.4 section 5.2.29: a processor hierarchy node is type 0 (length 20 + 4 * private
  resources: flags, parent offset, ACPI processor ID, then the offsets of its private resources);
  a cache type structure is type 1, 24 bytes, or 28 in table revision 3 (with a cache ID). Flags
  of a cache: bit 0 size, 1 sets, 2 associativity, 3 allocation type, 4 cache type, 5 write
  policy, 6 line size, 7 cache ID valid. Attributes: bits 1:0 allocation (1 read, 3 read/write),
  bits 3:2 type (0 data, 1 instruction, 2 unified), bit 4 write policy (0 write-back).
  Layout as Linux's ACPI cacheinfo reads it and ACPI 6.4 describes it: a leaf lists its private
  (per-core) caches' level 1, each cache points at the next level through "next level of cache",
  and the caches the cluster's cores share are the cluster node's private resources (they are
  private to the cluster), also reached from the last per-core level's "next level".

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include "PpttBuild.h"

#define PB_MAX_NODES  32u

typedef struct {
  pb_u32    OldOff, NewOff, Flags, OldParent, AcpiId, NumRefs, NewRefs;
  int       Target;                 // a core of the boot CPU's cluster
  int       IsCluster;              // the boot CPU's cluster node
} PB_NODE;

typedef struct {
  const PB_CACHE    *C;
  int               Core;           // index into the target cores, -1 = shared
  pb_u32            Off, Id;
} PB_OUTCACHE;

static pb_u32 Rd32 (const pb_u8 *p) { return p[0] | (p[1] << 8) | ((pb_u32)p[2] << 16) | ((pb_u32)p[3] << 24); }
static void Wr32 (pb_u8 *p, pb_u32 v) { p[0] = (pb_u8)v; p[1] = (pb_u8)(v >> 8); p[2] = (pb_u8)(v >> 16); p[3] = (pb_u8)(v >> 24); }
static void Wr16 (pb_u8 *p, pb_u32 v) { p[0] = (pb_u8)v; p[1] = (pb_u8)(v >> 8); }

//
// The cache that "next level" of (core, level, kind) points at: the same core's level + 1 if
// private, else the shared one; unified, or the same kind.
//
static pb_u32
NextOf (const PB_OUTCACHE *Oc, pb_u32 N, int Core, pb_u32 Level, pb_u32 Kind, pb_u32 PrivateLevels)
{
  pb_u32  i;
  int     Want = (Level + 1 <= PrivateLevels) ? Core : -1;

  for (i = 0; i < N; i++) {
    if ((Oc[i].C->Level == Level + 1) && (Oc[i].Core == Want) &&
        ((Oc[i].C->Kind == PB_UNIFIED) || (Oc[i].C->Kind == Kind))) {
      return Oc[i].Off;
    }
  }
  return 0;
}

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
  )
{
  PB_NODE      Node[PB_MAX_NODES];
  PB_OUTCACHE  Oc[PB_MAX_NODES * PB_MAX_CACHES];
  pb_u32       N = 0, NumOc = 0, Off, i, j, k, Rev, CacheLen, Cluster = 0, NumCores = 0, NumL1 = 0, NumShared = 0, Sum, End;
  int          Found = 0;

  *Why = 0;
  if ((InLen < 36) || (In[0] != 'P') || (In[1] != 'P') || (In[2] != 'T') || (In[3] != 'T') || (Rd32 (In + 4) != InLen)) {
    *Why = "not a PPTT";
    return 0;
  }

  Rev      = In[8];
  CacheLen = (Rev >= 3) ? 28u : 24u;

  //
  // Processor nodes only, none with private resources yet.
  //
  for (Off = 36; Off < InLen; Off += In[Off + 1]) {
    if ((Off + 2 > InLen) || (In[Off + 1] < 20) || (Off + In[Off + 1] > InLen)) {
      *Why = "bad structure length";
      return 0;
    }
    if (In[Off] != 0) {
      *Why = "the PPTT already has structures other than processor nodes";
      return 0;
    }
    if (N == PB_MAX_NODES) {
      *Why = "too many processor nodes";
      return 0;
    }
    Node[N].OldOff    = Off;
    Node[N].Flags     = Rd32 (In + Off + 4);
    Node[N].OldParent = Rd32 (In + Off + 8);
    Node[N].AcpiId    = Rd32 (In + Off + 12);
    Node[N].NumRefs   = Rd32 (In + Off + 16);
    Node[N].Target    = 0;
    Node[N].IsCluster = 0;
    Node[N].NewRefs   = 0;
    if ((Node[N].NumRefs != 0) || (In[Off + 1] != 20)) {
      *Why = "processor nodes already have private resources";
      return 0;
    }
    N++;
  }

  //
  // The boot CPU's cluster: the parent of the leaf (flags bit 3) whose valid ACPI ID (bit 1) is 0.
  //
  for (i = 0; i < N; i++) {
    if ((Node[i].Flags & 0x8u) && (Node[i].Flags & 0x2u) && (Node[i].AcpiId == 0)) {
      Cluster = Node[i].OldParent;
      Found   = 1;
    }
  }
  if (!Found || (Cluster == 0)) {
    *Why = "no leaf with ACPI processor ID 0 under a cluster";
    return 0;
  }
  for (i = 0; i < N; i++) {
    if ((Node[i].Flags & 0x8u) && (Node[i].Flags & 0x2u) && (Node[i].OldParent == Cluster)) {
      Node[i].Target = (int)++NumCores;          // 1-based core number
    }
    if (Node[i].OldOff == Cluster) {
      Node[i].IsCluster = 1;
    }
  }

  //
  // The caches must make sense: levels 1..7, line 16..2048 and a power of two, sets and ways set.
  //
  if ((NumCaches == 0) || (NumCaches > PB_MAX_CACHES)) {
    *Why = "no or too many caches";
    return 0;
  }
  if ((PrivateLevels < 1) || (PrivateLevels > 7)) {
    *Why = "the per-core levels must be 1..7";
    return 0;
  }
  for (j = 0; j < NumCaches; j++) {
    const PB_CACHE  *C = &Caches[j];
    if ((C->Level < 1) || (C->Level > 7) || (C->Kind > PB_UNIFIED) || (C->LineSize < 16) || (C->LineSize > 2048) ||
        (C->LineSize & (C->LineSize - 1)) || (C->Sets == 0) || (C->Ways == 0) || (C->Ways > 255) ||
        ((unsigned long long)C->LineSize * C->Sets * C->Ways > 0x4000000ull)) {
      *Why = "implausible cache geometry";
      return 0;
    }
    if (C->Level == 1) {
      NumL1++;
    }
    if (C->Level > PrivateLevels) {
      NumShared++;
    }
  }
  if (NumL1 == 0) {
    *Why = "no level-1 cache";
    return 0;
  }

  //
  // Layout: the header, the processor nodes (target leaves grow by one reference per level-1
  // cache, the cluster by one per shared cache), then per target core its private caches, then
  // the shared ones.
  //
  Off = 36;
  for (i = 0; i < N; i++) {
    Node[i].NewRefs = Node[i].Target ? NumL1 : (Node[i].IsCluster ? NumShared : 0);
    Node[i].NewOff  = Off;
    Off            += 20 + 4 * Node[i].NewRefs;
    if (20 + 4 * Node[i].NewRefs > 255) {
      *Why = "a processor node would be too long";
      return 0;
    }
  }
  for (k = 1; k <= NumCores; k++) {
    for (j = 0; j < NumCaches; j++) {
      if (Caches[j].Level <= PrivateLevels) {
        Oc[NumOc].C    = &Caches[j];
        Oc[NumOc].Core = (int)k;
        Oc[NumOc].Off  = Off;
        Oc[NumOc].Id   = NumOc + 1;
        NumOc++;
        Off += CacheLen;
      }
    }
  }
  for (j = 0; j < NumCaches; j++) {
    if (Caches[j].Level > PrivateLevels) {
      Oc[NumOc].C    = &Caches[j];
      Oc[NumOc].Core = -1;
      Oc[NumOc].Off  = Off;
      Oc[NumOc].Id   = NumOc + 1;
      NumOc++;
      Off += CacheLen;
    }
  }
  End = Off;
  if ((End > OutCap) || (End > 0xFFFFu)) {
    *Why = "output too large";
    return 0;
  }

  for (i = 0; i < End; i++) {
    Out[i] = 0;
  }
  for (i = 0; i < 36; i++) {
    Out[i] = In[i];
  }
  Wr32 (Out + 4, End);

  for (i = 0; i < N; i++) {
    pb_u8   *P      = Out + Node[i].NewOff;
    pb_u32  Parent  = 0;
    pb_u32  Refs    = 0;

    for (j = 0; j < N; j++) {
      if (Node[j].OldOff == Node[i].OldParent) {
        Parent = Node[j].NewOff;
      }
    }
    if ((Node[i].OldParent != 0) && (Parent == 0)) {
      *Why = "a parent offset points at no node";
      return 0;
    }
    P[0] = 0;
    P[1] = (pb_u8)(20 + 4 * Node[i].NewRefs);
    Wr32 (P + 4, Node[i].Flags);
    Wr32 (P + 8, Parent);
    Wr32 (P + 12, Node[i].AcpiId);
    if (Node[i].Target) {
      for (k = 0; k < NumOc; k++) {
        if ((Oc[k].Core == Node[i].Target) && (Oc[k].C->Level == 1)) {
          Wr32 (P + 20 + 4 * Refs, Oc[k].Off);
          Refs++;
        }
      }
    }
    if (Node[i].IsCluster) {
      for (k = 0; k < NumOc; k++) {
        if (Oc[k].Core < 0) {
          Wr32 (P + 20 + 4 * Refs, Oc[k].Off);
          Refs++;
        }
      }
    }
    Wr32 (P + 16, Refs);
  }

  for (k = 0; k < NumOc; k++) {
    const PB_CACHE  *C = Oc[k].C;
    pb_u8           *P = Out + Oc[k].Off;
    pb_u32          Alloc = (C->Kind == PB_INSTRUCTION) ? 0x1u : 0x3u;

    P[0] = 1;
    P[1] = (pb_u8)CacheLen;
    Wr32 (P + 4, (Rev >= 3) ? 0xFFu : 0x7Fu);
    Wr32 (P + 8, NextOf (Oc, NumOc, Oc[k].Core, C->Level, C->Kind, PrivateLevels));
    Wr32 (P + 12, (pb_u32)C->LineSize * C->Sets * C->Ways);
    Wr32 (P + 16, C->Sets);
    P[20] = (pb_u8)C->Ways;
    P[21] = (pb_u8)(Alloc | (C->Kind << 2));   // write-back
    Wr16 (P + 22, C->LineSize);
    if (Rev >= 3) {
      Wr32 (P + 24, Oc[k].Id);
    }
  }

  Out[9] = 0;
  for (Sum = 0, i = 0; i < End; i++) {
    Sum += Out[i];
  }
  Out[9] = (pb_u8)(0x100u - (Sum & 0xFFu));
  return End;
}
