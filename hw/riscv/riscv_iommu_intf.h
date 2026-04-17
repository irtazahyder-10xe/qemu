#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ============= AMBA 3 ABH-Lite Protocol ============= */

/* AHB-Lite 3 Protocol Macros */
/* HTRANS: Transfer Type */
#define AHB3L_HTRANS_IDLE       0x0U
#define AHB3L_HTRANS_BUSY       0x1U
#define AHB3L_HTRANS_NONSEQ     0x2U
#define AHB3L_HTRANS_SEQ        0x3U

/* HBURST: Burst Type */
#define AHB3L_HBURST_SINGLE     0x0U

/* HSIZE: Transfer Size */
#define AHB3L_HSIZE_32BIT       0x2U
#define AHB3L_HSIZE_64BIT       0x3U

/* HRESP: Response Type */
#define AHB3L_HRESP_OKAY        0x0U
#define AHB3L_HRESP_ERROR       0x1U

/* HREADY: Transfer Status */
#define AHB3L_HREADY_WAIT       0x0U
#define AHB3L_HREADY_DONE       0x1U

/* HWRITE: Transfer Direction */
#define AHB3L_HWRITE_READ       0x0U
#define AHB3L_HWRITE_WRITE      0x1U

/* HPROT: Protection Control Bits */
#define AHB3L_HPROT_DATA        0x1U /* Bit 0: 1=Data, 0=Opcode */
#define AHB3L_HPROT_PRIV        0x2U /* Bit 1: 1=Privileged, 0=User */
#define AHB3L_HPROT_DEFAULT     (AHB3L_HPROT_DATA | AHB3L_HPROT_PRIV)

#define DOUBLE_ACCESS(size) (size == 8)

#define MASTER_TRANS_BYTES  sizeof(ahb3lite_mtrans_s)
#define SLAVE_TRANS_BYTES   sizeof(ahb3lite_strans_s)

/* AHB3Lite Master Transaction */
typedef struct ahb3lite_mtrans_s {
    bool ahb3lite_hwrite;
    bool ahb3lite_hready;
    bool ahb3lite_hmastlock;

    uint8_t ahb3lite_hsize;
    uint8_t ahb3lite_hburst;
    uint8_t ahb3lite_htrans;
    uint8_t ahb3lite_hprot;

    uint32_t ahb3lite_haddr;
    uint64_t ahb3lite_hwdata;
} ahb3lite_mtrans_s;

/* AHB3Lite Slave Transaction */
typedef struct ahb3lite_strans_s {
    bool ahb3lite_hreadyout;
    bool ahb3lite_hresp;

    uint64_t ahb3lite_hrdata;
} ahb3lite_strans_s;

/* Convert AHB3Lite Master struct to a series of bytes */
inline void mtrans2bytes(const ahb3lite_mtrans_s *trans, uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        return;
    }

    memcpy(trans_buf, trans, MASTER_TRANS_BYTES);
}

/* Convert AHB3Lite Slave struct to a series of bytes */
inline void strans2bytes(const ahb3lite_strans_s *trans, uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        return;
    }

    memcpy(trans_buf, trans, SLAVE_TRANS_BYTES);
}

/* Convert series of bytes to AHB3Lite Master struct */
inline void mbytes2trans(ahb3lite_mtrans_s *trans, const uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        return;
    }

    memcpy(trans, trans_buf, MASTER_TRANS_BYTES);
}

/* Convert series of bytes to AHB3Lite Slave struct */
inline void sbytes2trans(ahb3lite_strans_s *trans, const uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        return;
    }

    memcpy(trans, trans_buf, SLAVE_TRANS_BYTES);
}
