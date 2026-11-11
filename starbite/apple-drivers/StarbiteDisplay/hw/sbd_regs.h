/* SPDX-License-Identifier: APSL-2.0 */
/*
 * Exynos 9810 display registers used by StarbiteDisplay. Offsets and bit positions are hardware facts;
 * NOTES.md lists the source of each ([V] verified in a source we hold, [S] stated by an earlier port's public driver,
 * [I] inferred). No code was taken from the GPL sources that document them.
 */
#ifndef SBD_REGS_H
#define SBD_REGS_H

/* ---- DECON_F (SBD_BLK_DECON) ---- */
#define DECON_GLOBAL_CONTROL            0x0000
#define  DECON_GC_CMD_MODE              (1u << 8)       /* MIPI command mode */
#define  DECON_GC_IDLE                  (1u << 5)
#define  DECON_GC_RUN                   (1u << 4)
#define DECON_INT_ENABLE                0x0040
#define  DECON_INT_FRAME_DONE           (1u << 13)
#define  DECON_INT_FRAME_START          (1u << 12)
#define  DECON_INT_EXTRA                (1u << 4)
#define  DECON_INT_EN                   (1u << 0)
#define  DECON_INT_ENABLE_MASK          0x3111u
#define DECON_EXTRA_INT_ENABLE          0x0044
#define  DECON_XINT_CONFLICT            (1u << 8)       /* resource conflict */
#define  DECON_XINT_TIMEOUT             (1u << 4)
#define DECON_INT_PENDING               0x004c          /* write 1 to clear */
#define DECON_EXTRA_INT_PENDING         0x0050          /* write 1 to clear */
#define DECON_SHADOW_REQ                0x0060          /* cleared by hardware when the update is taken */
#define  DECON_SHADOW_GLOBAL            (1u << 31)
#define  DECON_SHADOW_WINS              0x3fu
#define  DECON_SHADOW_ALL               (DECON_SHADOW_GLOBAL | DECON_SHADOW_WINS)
#define DECON_TRIG_CONTROL              0x0070
#define  DECON_HW_TRIG_ACTIVE_VALUE     (1u << 13)
#define  DECON_HW_TRIG_EDGE_POLARITY    (1u << 12)
#define  DECON_SW_TRIG_EN               (1u << 8)
#define  DECON_HW_TRIG_MASK             (1u << 4)       /* TE ignored: the panel shows its own memory */
#define  DECON_HW_TRIG_EN               (1u << 0)
#define DECON_DATA_PATH_0               0x0214          /* bit 4w: window w on */
#define DECON_DATA_PATH_1               0x0218          /* bits 4w..4w+2: window w's DMA channel */
#define DECON_FRAME_COUNT               0x02a0
#define DECON_WIN_START(w)              (0x1008u + 0x30u * (w)) /* y << 16 | x */
#define DECON_WIN_END(w)                (0x100cu + 0x30u * (w)) /* last line << 16 | last column */
#define DECON_WINDOWS                   6

/* ---- DPU_DMA (SBD_BLK_IDMA), per channel block ---- */
#define IDMA_CH(type)                   (0x1000u * ((type) + 1u))
#define IDMA_ENABLE                     0x0000
#define  IDMA_SFR_UPDATE_FORCE          (1u << 4)
#define  IDMA_OP_BUSY                   (1u << 2)
#define IDMA_IRQ                        0x0004
#define  IDMA_IRQ_CONFIG_ERROR          (1u << 21)
#define  IDMA_IRQ_READ_SLAVE_ERROR      (1u << 19)
#define  IDMA_IRQ_DEADLOCK              (1u << 17)
#define  IDMA_IRQ_ERRORS                (IDMA_IRQ_CONFIG_ERROR | IDMA_IRQ_READ_SLAVE_ERROR | IDMA_IRQ_DEADLOCK)
#define IDMA_IN_CON                     0x0008
#define  IDMA_IN_IC_MAX_MASK            (0xffu << 19)
#define  IDMA_IN_FORMAT_SHIFT           11
#define  IDMA_IN_FORMAT_MASK            (0x1fu << 11)
#define  IDMA_IN_ROT_FLIP_MASK          (7u << 8)
#define  IDMA_IN_AFBC_MASK              (3u << 6)
#define  IDMA_IN_BLOCK_EN               (1u << 3)
#define IDMA_OUT_CON                    0x000c
#define IDMA_SRC_SIZE                   0x0010          /* height << 16 | stride in pixels */
#define IDMA_SRC_OFFSET                 0x0014
#define IDMA_IMG_SIZE                   0x0018          /* height << 16 | width */
#define IDMA_BASE_Y                     0x0040          /* 32-bit physical address */
#define IDMA_BASE_C                     0x0044
#define IDMA_DEADLOCK_NUM               0x0050
#define IDMA_BUS_CON                    0x0054
#define IDMA_DYN_GATING                 0x0058
#define IDMA_REQ_DEST                   0x0068
#define IDMA_CFG_ERR                    0x0870          /* 11 bits */

/* IDMA_IN_CON format codes */
#define IDMA_FMT_ARGB8888               0
#define IDMA_FMT_ABGR8888               1
#define IDMA_FMT_RGBA8888               2
#define IDMA_FMT_BGRA8888               3
#define IDMA_FMT_XRGB8888               4
#define IDMA_FMT_ARGB2101010            16
#define IDMA_FMT_ABGR2101010            17
#define IDMA_FMT_RGBA2101010            18
#define IDMA_FMT_BGRA2101010            19

/* ---- DPP (SBD_BLK_DPP), per channel block ---- */
#define DPP_CH(type)                    (0x1000u * ((type) + 1u))
#define DPP_ENABLE                      0x0000
#define  DPP_SFR_UPDATE_FORCE           (1u << 4)
#define DPP_IRQ                         0x0004
#define  DPP_IRQ_CONFIG_ERROR           (1u << 21)
#define DPP_IN_CON                      0x0008
#define  DPP_IN_KEEP                    ((1u << 3) | 7u)        /* per-pixel alpha select, format */
#define DPP_IMG_SIZE                    0x0018
#define DPP_SCALED_SIZE                 0x002c
#define DPP_H_RATIO                     0x0044          /* source / destination, 12.20 */
#define DPP_V_RATIO                     0x0048
#define DPP_V_COEF(phase, tap, bank)    (0x200u + 4u * (phase) + 0x24u * (tap) + 0x200u * (bank))
#define DPP_H_COEF(phase, tap, bank)    (0x290u + 4u * (phase) + 0x24u * (tap) + 0x200u * (bank))
#define DPP_YH_POS                      0x05b0
#define DPP_YV_POS                      0x05b4
#define DPP_CH_POS                      0x05b8
#define DPP_CV_POS                      0x05bc
#define DPP_DYN_GATING                  0x0a54
#define DPP_CFG_ERR                     0x0d08          /* 5 bits */

/* Channel blocks in address order, and DATA_PATH_CONTROL_1's channel number for each. */
#define SBD_CH_G0                       0
#define SBD_CH_G1                       1
#define SBD_CH_VG0                      2
#define SBD_CH_VG1                      3
#define SBD_CH_VGF0                     4       /* has a scaler */
#define SBD_CH_VGF1                     5       /* scaler + HDR */
#define SBD_PATH_CH_VGF0                1       /* DATA_PATH_CONTROL_1 value for VGF0 */

/* ---- MIPI DSIM0 (SBD_BLK_DSIM) ---- */
#define DSIM_CLK_CTRL                   0x0020
#define  DSIM_TX_REQUEST_HSCLK          (1u << 20)      /* the loader left the link in HS */
#define DSIM_CONFIG                     0x004c
#define  DSIM_VIDEO_MODE                (1u << 18)
#define DSIM_INTSRC                     0x0050          /* write 1 to clear */
#define  DSIM_INT_PH_EMPTY              (1u << 28)      /* header FIFO drained: the packet went out */
#define  DSIM_INT_LPRX_TIMEOUT          (1u << 21)
#define  DSIM_INT_BTA_TIMEOUT           (1u << 20)
#define  DSIM_INT_RX_DONE               (1u << 18)
#define DSIM_PKTHDR                     0x0058
#define  DSIM_PKTHDR_KEEP               0xfe000000u     /* bits the vendor and S-Boot preserve */
#define  DSIM_PKTHDR_BTA                (1u << 24)
#define DSIM_PAYLOAD                    0x005c
#define DSIM_RXFIFO                     0x0060
#define DSIM_FIFOCTRL                   0x0068          /* read only here: never written */
#define  DSIM_FIFO_PH_COUNT(v)          (((v) >> 16) & 0x3fu)
#define  DSIM_FIFO_RX_EMPTY             (1u << 12)
#define  DSIM_FIFO_PH_FULL              (1u << 11)
#define  DSIM_FIFO_PH_EMPTY             (1u << 10)
#define  DSIM_FIFO_PL_FULL              (1u << 9)

/* MIPI DSI data types */
#define DSI_DCS_SHORT                   0x05
#define DSI_DCS_SHORT_PARAM             0x15
#define DSI_DCS_READ                    0x06
#define DSI_SET_MAX_RETURN              0x37
#define DSI_DCS_LONG                    0x39
#define DSI_RX_ACK_ERROR                0x02
#define DSI_RX_EOT                      0x08
#define DSI_RX_GEN_SHORT_1              0x11
#define DSI_RX_GEN_SHORT_2              0x12
#define DSI_RX_GEN_LONG                 0x1a
#define DSI_RX_DCS_LONG                 0x1c
#define DSI_RX_DCS_SHORT_1              0x21
#define DSI_RX_DCS_SHORT_2              0x22

#endif /* SBD_REGS_H */
