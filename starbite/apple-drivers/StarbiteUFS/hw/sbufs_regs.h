/* SPDX-License-Identifier: APSL-2.0 */
/*
 * StarbiteUFS: register and protocol constants of the Exynos 9810 UFS 2.1 host (UFSHCI 2.1, JESD223C) and of
 * UFS 2.1 UPIUs (JESD220C). Plain C, shared by the kext and the host tests. Sources for every fact: NOTES.md.
 */
#ifndef SBUFS_REGS_H
#define SBUFS_REGS_H

/* ---- register banks: the ADT "reg" entries of /arm-io/ufs, in order (tools/board/starlte.board) ---- */
#define SBUFS_BANK_HCI          0       /* 0x11120000, 0x200: standard UFSHCI */
#define SBUFS_BANK_VS           1       /* 0x11121100, 0x200: Exynos vendor HCI (vs_hci) */
#define SBUFS_BANK_UNIPRO       2       /* 0x11110000, 0x8000: never mapped by this driver */
#define SBUFS_BANK_UFSP         3       /* 0x11130000, 0x100: UFS protector, only ever read */

/* ---- UFSHCI standard registers (bank HCI) ---- */
#define SBUFS_CAP               0x00
#define SBUFS_VER               0x08
#define SBUFS_AHIT              0x18    /* auto-hibernate idle timer: 0 on Exynos 9810 (drops completions) */
#define SBUFS_IS                0x20    /* write 1 to clear */
#define SBUFS_IE                0x24
#define SBUFS_HCS               0x30
#define SBUFS_HCE               0x34
#define SBUFS_UTRLBA            0x50
#define SBUFS_UTRLBAU           0x54
#define SBUFS_UTRLDBR           0x58
#define SBUFS_UTRLCLR           0x5c
#define SBUFS_UTRLRSR           0x60

#define SBUFS_CAP_NUTRS(c)      (((c) & 0x1fu) + 1u)
#define SBUFS_CAP_64AS          (1u << 24)

#define SBUFS_HCS_DP            (1u << 0)
#define SBUFS_HCS_UTRLRDY       (1u << 1)

#define SBUFS_IS_UTRCS          (1u << 0)       /* transfer request completion */
#define SBUFS_IS_UE             (1u << 2)       /* UIC error */
#define SBUFS_IS_DFES           (1u << 11)      /* device fatal */
#define SBUFS_IS_UTPES          (1u << 12)      /* UTP error */
#define SBUFS_IS_HCFES          (1u << 16)      /* host controller fatal */
#define SBUFS_IS_SBFES          (1u << 17)      /* system bus fatal */
#define SBUFS_IS_FATAL          (SBUFS_IS_DFES | SBUFS_IS_UTPES | SBUFS_IS_HCFES | SBUFS_IS_SBFES)
#define SBUFS_IE_WANTED         (SBUFS_IS_UTRCS | SBUFS_IS_UE | SBUFS_IS_FATAL)

/* ---- Exynos vendor HCI (bank VS, offsets from 0x11121100) ---- */
#define SBUFS_VS_TXPRDT_ENTRY_SIZE      0x00
#define SBUFS_VS_RXPRDT_ENTRY_SIZE      0x04
#define SBUFS_VS_UTRL_NEXUS_TYPE        0x40    /* bit n set: slot n carries a SCSI command */
#define SBUFS_VS_DATA_REORDER           0x60
#define SBUFS_VS_AXIDMA_RWDATA_BURST    0x6c
/* values both Linux and the exynos9810-woa UEFI program, needed for multi-entry PRDTs (herolte_ufs.c, phone) */
#define SBUFS_VS_PRDT_PREFETCH_4K       0x8000000cu     /* PRDT_PREFECT_EN | PRDT_SET_SIZE(12) */
#define SBUFS_VS_REORDER_VAL            0x0000000au
#define SBUFS_VS_AXIDMA_VAL             0x98000003u     /* WLU_EN | BURST_LEN(3) */

/* ---- UFS protector (bank UFSP), read for the log only ---- */
#define SBUFS_UFSP_RCTRL        0x000
#define SBUFS_UFSP_RSECURITY    0x010
#define SBUFS_UFSP_REGION(n)    (0x200u + (n) * 0x10u)  /* begin, end, lun, ctrl */

/* ---- UTP transfer request descriptor (32 bytes) ---- */
#define SBUFS_UTRD_CT_UFS       (1u << 28)      /* command type: UFS storage */
#define SBUFS_UTRD_DD_NONE      (0u << 25)
#define SBUFS_UTRD_DD_WRITE     (1u << 25)      /* system memory to device */
#define SBUFS_UTRD_DD_READ      (2u << 25)      /* device to system memory */
#define SBUFS_UTRD_DD_MASK      (3u << 25)
#define SBUFS_UTRD_I            (1u << 24)      /* interrupt on completion */
#define SBUFS_OCS_SUCCESS       0x00u
#define SBUFS_OCS_INVALID       0x0fu

/* ---- UPIU ---- */
#define SBUFS_UPIU_COMMAND      0x01
#define SBUFS_UPIU_RESPONSE     0x21
#define SBUFS_UPIU_FLAG_W       0x20
#define SBUFS_UPIU_FLAG_R       0x40
#define SBUFS_RESP_FLAG_U       0x20            /* underflow: residual count valid */
#define SBUFS_RESP_FLAG_O       0x40            /* overflow */

/* well-known LUs (JESD220C 10.8.5); only the device W-LUN is ever addressed, and only to wake it */
#define SBUFS_WLUN_REPORT_LUNS  0x81
#define SBUFS_WLUN_BOOT         0xb0
#define SBUFS_WLUN_RPMB         0xc4
#define SBUFS_WLUN_DEVICE       0xd0
#define SBUFS_MAX_GENERAL_LU    8u              /* general LUs 0..7 */

/* ---- SCSI ---- */
#define SBUFS_SCSI_TUR          0x00
#define SBUFS_SCSI_INQUIRY      0x12
#define SBUFS_SCSI_SSU          0x1b
#define SBUFS_SCSI_READ_CAP10   0x25
#define SBUFS_SCSI_READ10       0x28
#define SBUFS_SCSI_WRITE10      0x2a
#define SBUFS_SCSI_SYNC10       0x35
#define SBUFS_SCSI_READ16       0x88
#define SBUFS_SCSI_WRITE16      0x8a
#define SBUFS_SCSI_SYNC16       0x91
#define SBUFS_SCSI_SAI16        0x9e            /* SERVICE ACTION IN(16) */
#define SBUFS_SAI_READ_CAP16    0x10

#define SBUFS_STATUS_GOOD       0x00
#define SBUFS_STATUS_CHECK      0x02
#define SBUFS_SENSE_NOT_READY   0x02
#define SBUFS_SENSE_UNIT_ATTN   0x06

/* ---- geometry ---- */
#define SBUFS_BLOCK             4096u           /* the device's logical block (READ CAPACITY must agree) */
#define SBUFS_PRD_MAX_BYTES     4096u           /* the protector's data unit: one PRDT entry carries at most this */

#endif /* SBUFS_REGS_H */
