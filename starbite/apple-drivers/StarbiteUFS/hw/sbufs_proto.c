/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS protocol (sbufs_proto.h). Layouts: UFSHCI 2.1 (JESD223C) 6.1 (UTRD), 6.1.2 (PRDT); UFS 2.1
 * (JESD220C) 10.7.1 (COMMAND UPIU), 10.7.2 (RESPONSE UPIU). Exynos differences (offsets/lengths in bytes, 4 KiB
 * per PRDT entry, the vendor nexus bit) as herolte_ufs.c runs them on the phone; see NOTES.md.
 */
#include "sbufs_proto.h"

static void
put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void
sbufs_layout_s9(struct sbufs_layout *L, uint32_t max_entries, bool dma64)
{
	L->rsp_off = 512;
	L->rsp_len = 512;
	L->prdt_off = 1024;
	L->ucd_size = SBUFS_UCD_SIZE;
	L->stride = 16;
	L->max_entries = max_entries ? max_entries : 1;
	if (L->max_entries > (L->ucd_size - L->prdt_off) / L->stride) {
		L->max_entries = (L->ucd_size - L->prdt_off) / L->stride;
	}
	L->byte_gran = true;
	L->dma_limit = dma64 ? 0 : (1ull << 32);
}

static void
cmd_init(struct sbufs_cmd *c, uint8_t lun, uint8_t dir, uint8_t cdb_len, uint32_t xfer)
{
	memset(c, 0, sizeof(*c));
	c->lun = lun;
	c->dir = dir;
	c->cdb_len = cdb_len;
	c->xfer_len = xfer;
}

static int
rw(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks, bool write, bool fua)
{
	if (nblks == 0 || nblks > UINT32_MAX / SBUFS_BLOCK || lba > UINT64_MAX - nblks) {
		return SBUFS_E_INVAL;
	}
	uint32_t xfer = nblks * SBUFS_BLOCK;
	if (lba + nblks - 1 <= UINT32_MAX && nblks <= 0xffffu) {
		cmd_init(c, lun, write ? SBUFS_DIR_WRITE : SBUFS_DIR_READ, 10, xfer);
		c->cdb[0] = write ? SBUFS_SCSI_WRITE10 : SBUFS_SCSI_READ10;
		sbufs_put_be32(&c->cdb[2], (uint32_t)lba);
		sbufs_put_be16(&c->cdb[7], (uint16_t)nblks);
	} else {
		cmd_init(c, lun, write ? SBUFS_DIR_WRITE : SBUFS_DIR_READ, 16, xfer);
		c->cdb[0] = write ? SBUFS_SCSI_WRITE16 : SBUFS_SCSI_READ16;
		sbufs_put_be64(&c->cdb[2], lba);
		sbufs_put_be32(&c->cdb[10], nblks);
	}
	if (write && fua) {
		c->cdb[1] = 0x08;
	}
	return SBUFS_OK;
}

int
sbufs_cdb_read(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks)
{
	return rw(c, lun, lba, nblks, false, false);
}

int
sbufs_cdb_write(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks, bool fua)
{
	return rw(c, lun, lba, nblks, true, fua);
}

int
sbufs_cdb_sync(struct sbufs_cmd *c, uint8_t lun, uint64_t lba, uint32_t nblks)
{
	/* never 0 blocks: SYNCHRONIZE CACHE with 0 means "to the end of the LU" */
	if (nblks == 0 || lba > UINT64_MAX - nblks) {
		return SBUFS_E_INVAL;
	}
	if (lba + nblks - 1 <= UINT32_MAX && nblks <= 0xffffu) {
		cmd_init(c, lun, SBUFS_DIR_NONE, 10, 0);
		c->cdb[0] = SBUFS_SCSI_SYNC10;
		sbufs_put_be32(&c->cdb[2], (uint32_t)lba);
		sbufs_put_be16(&c->cdb[7], (uint16_t)nblks);
	} else {
		cmd_init(c, lun, SBUFS_DIR_NONE, 16, 0);
		c->cdb[0] = SBUFS_SCSI_SYNC16;
		sbufs_put_be64(&c->cdb[2], lba);
		sbufs_put_be32(&c->cdb[10], nblks);
	}
	return SBUFS_OK;
}

void
sbufs_cdb_tur(struct sbufs_cmd *c, uint8_t lun)
{
	cmd_init(c, lun, SBUFS_DIR_NONE, 6, 0);
	c->cdb[0] = SBUFS_SCSI_TUR;
}

void
sbufs_cdb_inquiry(struct sbufs_cmd *c, uint8_t lun, uint8_t alloc)
{
	cmd_init(c, lun, SBUFS_DIR_READ, 6, alloc);
	c->cdb[0] = SBUFS_SCSI_INQUIRY;
	sbufs_put_be16(&c->cdb[3], alloc);
}

void
sbufs_cdb_read_cap10(struct sbufs_cmd *c, uint8_t lun)
{
	cmd_init(c, lun, SBUFS_DIR_READ, 10, 8);
	c->cdb[0] = SBUFS_SCSI_READ_CAP10;
}

void
sbufs_cdb_read_cap16(struct sbufs_cmd *c, uint8_t lun)
{
	cmd_init(c, lun, SBUFS_DIR_READ, 16, 32);
	c->cdb[0] = SBUFS_SCSI_SAI16;
	c->cdb[1] = SBUFS_SAI_READ_CAP16;
	sbufs_put_be32(&c->cdb[10], 32);
}

void
sbufs_cdb_ssu_active(struct sbufs_cmd *c)
{
	cmd_init(c, SBUFS_WLUN_DEVICE, SBUFS_DIR_NONE, 6, 0);
	c->cdb[0] = SBUFS_SCSI_SSU;
	c->cdb[4] = 0x10;               /* POWER CONDITION 1 = ACTIVE, START 0 */
}

int
sbufs_build(const struct sbufs_layout *L, uint8_t *utrd, uint8_t *ucd, uint64_t ucd_pa, const struct sbufs_cmd *c,
    uint8_t tag, const struct sbufs_seg *segs, unsigned nsegs, bool irq)
{
	uint32_t total = 0;

	if ((ucd_pa & 0x7f) != 0 || c->cdb_len > 16 || (c->xfer_len == 0) != (c->dir == SBUFS_DIR_NONE)) {
		return SBUFS_E_INVAL;
	}
	if (c->dir != SBUFS_DIR_NONE) {
		if (segs == NULL || nsegs == 0 || nsegs > L->max_entries) {
			return SBUFS_E_DMA;
		}
		for (unsigned i = 0; i < nsegs; i++) {
			uint64_t pa = segs[i].pa;
			uint32_t len = segs[i].len;
			if (len == 0 || len > SBUFS_PRD_MAX_BYTES || (len & 3) != 0 || (pa & 3) != 0 ||
			    pa > UINT64_MAX - len || (L->dma_limit != 0 && pa + len > L->dma_limit) ||
			    total > UINT32_MAX - len) {
				return SBUFS_E_DMA;
			}
			total += len;
		}
		if (total != c->xfer_len) {
			return SBUFS_E_DMA;
		}
	} else if (nsegs != 0) {
		return SBUFS_E_INVAL;
	}

	memset(ucd, 0, L->prdt_off + (size_t)nsegs * L->stride);
	ucd[0] = SBUFS_UPIU_COMMAND;
	ucd[1] = c->dir == SBUFS_DIR_READ ? SBUFS_UPIU_FLAG_R : c->dir == SBUFS_DIR_WRITE ? SBUFS_UPIU_FLAG_W : 0;
	ucd[2] = c->lun;
	ucd[3] = tag;
	sbufs_put_be32(&ucd[12], c->xfer_len);
	memcpy(&ucd[16], c->cdb, c->cdb_len);
	for (unsigned i = 0; i < nsegs; i++) {
		uint8_t *e = ucd + L->prdt_off + (size_t)i * L->stride;
		put_le32(e + 0, (uint32_t)segs[i].pa);
		put_le32(e + 4, (uint32_t)(segs[i].pa >> 32));
		put_le32(e + 8, 0);
		put_le32(e + 12, (segs[i].len - 1) & 0x3ffffu);  /* bits 28-31 (FMP FAS/DAS) 0: no encryption */
	}

	uint32_t dd = c->dir == SBUFS_DIR_READ ? SBUFS_UTRD_DD_READ : c->dir == SBUFS_DIR_WRITE ? SBUFS_UTRD_DD_WRITE :
	    SBUFS_UTRD_DD_NONE;
	uint32_t rsp = L->byte_gran ? (L->rsp_off << 16 | L->rsp_len) : ((L->rsp_off / 4) << 16 | (L->rsp_len / 4));
	uint32_t prdt = L->byte_gran ? (L->prdt_off << 16 | (nsegs * L->stride)) : ((L->prdt_off / 4) << 16 | nsegs);
	put_le32(utrd + 0, SBUFS_UTRD_CT_UFS | dd | (irq ? SBUFS_UTRD_I : 0));
	put_le32(utrd + 4, 0);
	put_le32(utrd + 8, SBUFS_OCS_INVALID);
	put_le32(utrd + 12, 0);
	put_le32(utrd + 16, (uint32_t)ucd_pa);
	put_le32(utrd + 20, (uint32_t)(ucd_pa >> 32));
	put_le32(utrd + 24, rsp);
	put_le32(utrd + 28, prdt);
	return SBUFS_OK;
}

static int
refuse(struct sbufs_decoded *d, const char *why)
{
	d->why = why;
	return SBUFS_E_GATE;
}

static bool
zero_from(const uint8_t *cdb, unsigned from)
{
	for (unsigned i = from; i < 16; i++) {
		if (cdb[i] != 0) {
			return false;
		}
	}
	return true;
}

int
sbufs_gate(const struct sbufs_layout *L, const uint8_t *utrd, const uint8_t *ucd, const struct sbufs_fence *f,
    struct sbufs_decoded *d)
{
	const uint8_t *cdb = ucd + 16;
	uint32_t dw0 = sbufs_le32(utrd), dw6 = sbufs_le32(utrd + 24), dw7 = sbufs_le32(utrd + 28);
	uint32_t want_rsp = L->byte_gran ? (L->rsp_off << 16 | L->rsp_len) : ((L->rsp_off / 4) << 16 | (L->rsp_len / 4));
	uint32_t n, total = 0;

	memset(d, 0, sizeof(*d));
	d->opcode = cdb[0];
	d->lun = ucd[2];
	d->xfer_len = sbufs_be32(ucd + 12);
	if ((dw0 >> 28) != 1 || (dw0 & 0x00ffffffu & ~SBUFS_UTRD_I) != 0 || (dw0 & (1u << 27)) != 0) {
		return refuse(d, "UTRD: not a plain UFS storage command");
	}
	if (ucd[0] != SBUFS_UPIU_COMMAND || (ucd[1] & ~(SBUFS_UPIU_FLAG_R | SBUFS_UPIU_FLAG_W)) != 0 ||
	    ucd[4] != 0 || ucd[5] != 0 || ucd[6] != 0 || ucd[7] != 0 || sbufs_be32(ucd + 8) != 0) {
		return refuse(d, "UPIU: not a plain SCSI COMMAND UPIU");
	}
	switch (dw0 & SBUFS_UTRD_DD_MASK) {
	case SBUFS_UTRD_DD_NONE: d->dir = SBUFS_DIR_NONE; break;
	case SBUFS_UTRD_DD_READ: d->dir = SBUFS_DIR_READ; break;
	case SBUFS_UTRD_DD_WRITE: d->dir = SBUFS_DIR_WRITE; break;
	default: return refuse(d, "UTRD: bad data direction");
	}
	uint8_t want_flags = d->dir == SBUFS_DIR_READ ? SBUFS_UPIU_FLAG_R : d->dir == SBUFS_DIR_WRITE ? SBUFS_UPIU_FLAG_W : 0;
	if (ucd[1] != want_flags || (d->dir == SBUFS_DIR_NONE) != (d->xfer_len == 0)) {
		return refuse(d, "UPIU flags / length disagree with the UTRD direction");
	}
	if (dw6 != want_rsp || (dw7 >> 16) != (L->byte_gran ? L->prdt_off : L->prdt_off / 4)) {
		return refuse(d, "UTRD: response/PRDT offsets not ours");
	}
	if (L->byte_gran) {
		if ((dw7 & 0xffffu) % L->stride != 0) {
			return refuse(d, "UTRD: PRDT length not a whole number of entries");
		}
		n = (dw7 & 0xffffu) / L->stride;
	} else {
		n = dw7 & 0xffffu;
	}
	if (n > L->max_entries || (n == 0) != (d->xfer_len == 0)) {
		return refuse(d, "PRDT: entry count");
	}
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = ucd + L->prdt_off + (size_t)i * L->stride;
		uint32_t dbc = sbufs_le32(e + 12);
		if ((dbc & ~0x3ffffu) != 0 || dbc + 1 > SBUFS_PRD_MAX_BYTES || (sbufs_le32(e) & 3) != 0) {
			return refuse(d, "PRDT: entry too large, encrypted or misaligned");
		}
		total += dbc + 1;
	}
	if (total != d->xfer_len) {
		return refuse(d, "PRDT: lengths do not add up to the expected transfer");
	}

	bool wlun = d->lun == SBUFS_WLUN_DEVICE;
	if (!wlun && d->lun >= SBUFS_MAX_GENERAL_LU) {
		return refuse(d, "LUN: not a general LU nor the device W-LUN");
	}
	if (wlun && cdb[0] != SBUFS_SCSI_TUR && cdb[0] != SBUFS_SCSI_SSU) {
		return refuse(d, "device W-LUN: only TEST UNIT READY and START STOP UNIT (ACTIVE)");
	}
	switch (cdb[0]) {
	case SBUFS_SCSI_TUR:
		if (d->dir != SBUFS_DIR_NONE || !zero_from(cdb, 1)) {
			return refuse(d, "TEST UNIT READY: malformed");
		}
		return SBUFS_OK;
	case SBUFS_SCSI_SSU:
		if (!wlun || d->dir != SBUFS_DIR_NONE || (cdb[1] & ~1u) != 0 || cdb[2] != 0 || cdb[3] != 0 ||
		    cdb[4] != 0x10 || !zero_from(cdb, 5)) {
			return refuse(d, "START STOP UNIT: only ACTIVE to the device W-LUN");
		}
		return SBUFS_OK;
	case SBUFS_SCSI_INQUIRY:
		if (d->dir != SBUFS_DIR_READ || cdb[1] != 0 || cdb[2] != 0 || sbufs_be16(cdb + 3) != d->xfer_len ||
		    d->xfer_len > 255 || !zero_from(cdb, 5)) {
			return refuse(d, "INQUIRY: malformed");
		}
		return SBUFS_OK;
	case SBUFS_SCSI_READ_CAP10:
		if (d->dir != SBUFS_DIR_READ || d->xfer_len != 8 || !zero_from(cdb, 1)) {
			return refuse(d, "READ CAPACITY(10): malformed");
		}
		return SBUFS_OK;
	case SBUFS_SCSI_SAI16:
		if ((cdb[1] & 0x1f) != SBUFS_SAI_READ_CAP16 || d->dir != SBUFS_DIR_READ || d->xfer_len != 32 ||
		    sbufs_be32(cdb + 10) != 32 || cdb[2] | cdb[3] | cdb[4] | cdb[5] | cdb[6] | cdb[7] | cdb[8] | cdb[9] ||
		    !zero_from(cdb, 14)) {
			return refuse(d, "SERVICE ACTION IN: only READ CAPACITY(16)");
		}
		return SBUFS_OK;
	case SBUFS_SCSI_READ10:
	case SBUFS_SCSI_WRITE10:
	case SBUFS_SCSI_SYNC10:
		d->lba = sbufs_be32(cdb + 2);
		d->nblks = sbufs_be16(cdb + 7);
		if (cdb[6] != 0 || !zero_from(cdb, 9)) {
			return refuse(d, "10-byte CDB: group/control/trailing bytes set");
		}
		break;
	case SBUFS_SCSI_READ16:
	case SBUFS_SCSI_WRITE16:
	case SBUFS_SCSI_SYNC16:
		d->lba = sbufs_be64(cdb + 2);
		d->nblks = sbufs_be32(cdb + 10);
		if (cdb[14] != 0 || cdb[15] != 0) {
			return refuse(d, "16-byte CDB: group/control set");
		}
		break;
	default:
		return refuse(d, "opcode not on the whitelist");
	}
	d->data = true;
	bool rd = cdb[0] == SBUFS_SCSI_READ10 || cdb[0] == SBUFS_SCSI_READ16;
	bool sync = cdb[0] == SBUFS_SCSI_SYNC10 || cdb[0] == SBUFS_SCSI_SYNC16;
	d->write = !rd;
	if (d->nblks == 0) {
		return refuse(d, "zero blocks (SYNCHRONIZE CACHE 0 = the whole LU)");
	}
	if (rd && (d->dir != SBUFS_DIR_READ || cdb[1] != 0)) {
		return refuse(d, "READ: direction or flags");
	}
	if (!rd && !sync && (d->dir != SBUFS_DIR_WRITE || (cdb[1] & ~0x08u) != 0)) {
		return refuse(d, "WRITE: direction or flags (FUA only)");
	}
	if (sync && (d->dir != SBUFS_DIR_NONE || (cdb[1] & ~0x02u) != 0)) {
		return refuse(d, "SYNCHRONIZE CACHE: direction or flags (IMMED only)");
	}
	if (!sync && (d->nblks > UINT32_MAX / SBUFS_BLOCK || d->xfer_len != d->nblks * SBUFS_BLOCK)) {
		return refuse(d, "expected length is not blocks x 4096");
	}
	if (sbufs_fence_check_quiet(f, d->lun, d->lba, d->nblks, d->write) != SBUFS_FENCE_OK) {
		return refuse(d, "fence: outside the Starbite-owned ranges or not writable");
	}
	return SBUFS_OK;
}

void
sbufs_parse_response(const struct sbufs_layout *L, const uint8_t *utrd, const uint8_t *ucd, uint8_t tag,
    struct sbufs_result *r)
{
	const uint8_t *rsp = ucd + L->rsp_off;
	memset(r, 0, sizeof(*r));
	r->ocs = (uint8_t)(sbufs_le32(utrd + 8) & 0xff);
	r->upiu = rsp[0];
	r->response = rsp[6];
	r->status = rsp[7];
	r->residual = sbufs_be32(rsp + 12);
	r->sense_key = r->asc = r->ascq = 0xff;
	uint16_t slen = sbufs_be16(rsp + 32);
	if (slen > L->rsp_len - 34) {
		slen = (uint16_t)(L->rsp_len - 34);
	}
	if (slen >= 3) {
		r->sense_key = rsp[34 + 2] & 0x0f;
	}
	if (slen >= 14) {
		r->asc = rsp[34 + 12];
		r->ascq = rsp[34 + 13];
	}
	if (r->ocs != SBUFS_OCS_SUCCESS || r->upiu != SBUFS_UPIU_RESPONSE || rsp[3] != tag || r->response != 0 ||
	    r->status != SBUFS_STATUS_GOOD) {
		r->err = SBUFS_E_IO;
	} else if ((rsp[1] & (SBUFS_RESP_FLAG_U | SBUFS_RESP_FLAG_O)) != 0 && r->residual != 0) {
		r->err = SBUFS_E_IO;            /* short or long transfer */
	} else {
		r->err = SBUFS_OK;
	}
}
