/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sbm: stage 5b, the RFS server's core (docs/MODEM.md 7.2). The CP reads and writes its NV data over ch 245; on
 * Android, libsec-ril's Nv::ProcessRfsMessage serves it from /mnt/vendor/efs. Here the NV data is a RAM copy
 * (read once, read only, from EFS's nv_data.bin): reads come from it, writes go **into the RAM copy only**, never
 * back to a partition, and are lost at power-off on purpose.
 *
 * The protocol, read from this phone's One UI libsec-ril.so (vendor partition, 2026-11-09):
 *   request / response header: u32 length (the whole message), u8 index, u8 id (the request's id is echoed)
 *   index 0x01 NV_READ:  request  + u32 offset, u32 length (<= 0x80000)
 *                        response + u8 confirm (1 ok, 0 failed), u32 offset, u32 length, data      (15 + n bytes)
 *   index 0x02 NV_WRITE: request  + u32 offset, u32 length, data
 *                        response + u8 confirm, u32 offset, u32 length                            (15 bytes)
 *   0x03 read file, 0x04 write file, 0x05 lseek, 0x06 close, 0x07 put file, 0x08 get file, 0x09 rename,
 *   0x0a get file info, 0x0b delete, 0x0c make directory, 0x11 open (u32 flags, u32 path_len, path),
 *   0x13 file info by handle, 0x14 create, 0x15 NV write all, 0x17 NV restore; other indexes are refused.
 * This core answers NV_READ and NV_WRITE; the file operations are counted and reported until T9's log shows which
 * ones the CP uses at boot (their reply layouts are still to be read from the library).
 */
#include "sbm.h"

void sbm_rfs_init(struct sbm_rfs *r, uint8_t *nv, uint32_t nv_size)
{
    sbm_memset(r, 0, sizeof(*r));
    r->nv = nv;
    r->nv_size = nv_size;
}

static int reply_hdr(uint8_t *out, uint32_t cap, uint32_t len, uint8_t idx, uint8_t id)
{
    if (cap < len)
        return SBM_ENOSPC;
    sbm_put32(out, len);
    out[4] = idx;
    out[5] = id;
    return 0;
}

int sbm_rfs_handle(struct sbm_rfs *r, const uint8_t *req, uint32_t n, uint8_t *out, uint32_t cap)
{
    if (!req || n < SBM_RFS_HDR || sbm_le32(req) != n) {
        r->bad++;
        return SBM_EBADF;
    }
    uint8_t idx = req[4], id = req[5];
    switch (idx) {
    case SBM_RFS_IDX_NV_READ: {
        if (n < SBM_RFS_HDR + 8) {
            r->bad++;
            return SBM_EBADF;
        }
        uint32_t off = sbm_le32(req + 6), len = sbm_le32(req + 10);
        int ok = r->nv && off <= r->nv_size && len <= r->nv_size - off && len <= SBM_RFS_MAX_IO;
        uint32_t got = ok ? len : 0;
        if (cap < SBM_RFS_REPLY_HDR + got)
            return SBM_ENOSPC;
        reply_hdr(out, cap, SBM_RFS_REPLY_HDR + got, idx, id);
        out[6] = (uint8_t)ok;
        sbm_put32(out + 7, off);
        sbm_put32(out + 11, got);
        if (got)
            sbm_memcpy(out + SBM_RFS_REPLY_HDR, r->nv + off, got);
        r->reads++;
        r->read_bytes += got;
        if (!ok)
            r->refused++;
        return (int)(SBM_RFS_REPLY_HDR + got);
    }
    case SBM_RFS_IDX_NV_WRITE: {
        if (n < SBM_RFS_HDR + 8) {
            r->bad++;
            return SBM_EBADF;
        }
        uint32_t off = sbm_le32(req + 6), len = sbm_le32(req + 10);
        int ok = r->nv && len <= n - (SBM_RFS_HDR + 8) && off <= r->nv_size && len <= r->nv_size - off &&
                 len <= SBM_RFS_MAX_IO;
        if (ok) {
            sbm_memcpy(r->nv + off, req + SBM_RFS_HDR + 8, len);      /* the RAM copy only */
            r->write_bytes += len;
        } else {
            r->refused++;
        }
        int e = reply_hdr(out, cap, SBM_RFS_REPLY_HDR, idx, id);
        if (e < 0)
            return e;
        out[6] = (uint8_t)ok;
        sbm_put32(out + 7, off);
        sbm_put32(out + 11, ok ? len : 0);
        r->writes++;
        return SBM_RFS_REPLY_HDR;
    }
    default:
        if (idx < sizeof(r->other) / sizeof(r->other[0]))
            r->other[idx]++;
        r->unhandled++;
        return 0;               /* not answered (yet): T9 shows which of these the CP needs */
    }
}
