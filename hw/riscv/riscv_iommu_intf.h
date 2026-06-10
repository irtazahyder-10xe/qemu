#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// TODO: Update naming scheme for all protocols to be consistent
/* ============= AMBA 3 ABH-Lite Protocol ============= */

/* AHB-Lite 3 Protocol Macros */
/* HTRANS: Transfer Type */
#define AHB3L_HTRANS_MASK       0x3U
#define AHB3L_HTRANS_IDLE       0x0U
#define AHB3L_HTRANS_BUSY       0x1U
#define AHB3L_HTRANS_NONSEQ     0x2U
#define AHB3L_HTRANS_SEQ        0x3U

/* HBURST: Burst Type */
#define AHB3L_HBURST_MASK       0x7U
#define AHB3L_HBURST_SINGLE     0x0U

/* HSIZE: Transfer Size */
#define AHB3L_HSIZE_MASK        0x7U
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
#define AHB3L_HPROT_MASK        0xFU
#define AHB3L_HPROT_DATA        0x1U /* Bit 0: 1=Data, 0=Opcode */
#define AHB3L_HPROT_PRIV        0x2U /* Bit 1: 1=Privileged, 0=User */
#define AHB3L_HPROT_DEFAULT     (AHB3L_HPROT_DATA | AHB3L_HPROT_PRIV)

#define DOUBLE_ACCESS(size) (size == 8)

#define MASTER_TRANS_BYTES  sizeof(ahb3lite_mtrans_s)
#define SLAVE_TRANS_BYTES   sizeof(ahb3lite_strans_s)

/* AHB3Lite Master Transaction */
typedef struct {
    bool ahb3lite_hwrite;
    bool ahb3lite_hready;
    bool ahb3lite_hmastlock;
    bool ahb3lite_hsel;

    /* We are using 32 bits to ensure alignment with SV svBitVecVal */
    uint32_t ahb3lite_hsize;
    uint32_t ahb3lite_hburst;
    uint32_t ahb3lite_htrans;
    uint32_t ahb3lite_hprot;

    uint32_t ahb3lite_haddr;
    uint64_t ahb3lite_hwdata;
} ahb3lite_mtrans_s;

/* AHB3Lite Slave Transaction */
typedef struct {
    bool ahb3lite_hreadyout;
    bool ahb3lite_hresp;

    uint64_t ahb3lite_hrdata;
} ahb3lite_strans_s;

/* Convert AHB3Lite Master struct to a series of bytes */
inline void mtrans2bytes(const ahb3lite_mtrans_s *trans, uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        trans_buf = NULL;
        return;
    }

    memcpy(trans_buf, trans, MASTER_TRANS_BYTES);
}

/* Convert AHB3Lite Slave struct to a series of bytes */
inline void strans2bytes(const ahb3lite_strans_s *trans, uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        trans_buf = NULL;
        return;
    }

    memcpy(trans_buf, trans, SLAVE_TRANS_BYTES);
}

/* Convert series of bytes to AHB3Lite Master struct */
inline void mbytes2trans(ahb3lite_mtrans_s *trans, const uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        trans = NULL;
        return;
    }

    memcpy(trans, trans_buf, MASTER_TRANS_BYTES);
}

/* Convert series of bytes to AHB3Lite Slave struct */
inline void sbytes2trans(ahb3lite_strans_s *trans, const uint8_t *trans_buf)
{
    if (!trans || !trans_buf) {
        trans = NULL;
        return;
    }

    memcpy(trans, trans_buf, SLAVE_TRANS_BYTES);
}

/* ============= AMBA LTI A Protocol ============= */
/** @name Request Channel Signals
 *
 * LTI_LRUSER: {NPPN[43:0],NID[10:0]}
 * @{
 */
#define LTI_LASID_DEVID_MASK    0xFFFFFFUL
#define LTI_LASSID_PROCID_MASK  0xFFFFFUL
/** @} */

/** @name Response Channel Signals
 *
 * LTI_LRUSER: {NPPN[43:0],NID[10:0]}
 * @{
 */
#define LTI_LRUSER_NPPN_MASK    0xFFFFFFFFFFFUL
#define LTI_LRUSER_NPPN_OFFSET  0xB
#define LTI_LRUSER_NID_MASK     0x7FFUL
#define LTI_LUSER_MASK          ((LTI_LRUSER_NPPN_MASK << LTI_LRUSER_NPPN_OFFSET) | LTI_LRUSER_NID_MASK)
/** @} */

/* LTI Flow: For now only LTI_ATST and LTI_NO_STALL are supported */
typedef enum {
    LTI_FLOW_ATST = 1,
    LTI_FLOW_NO_STALL = 2
} lti_laflow_t;

/* LTI Resp: Response can be one of Succes, MRIF Success or FAULT_ABORT */
typedef enum {
    LTI_RESP_SUCCESS = 0,
    LTI_RESP_MRIF_SUCCESS = 3,
    LTI_RESP_FAULT_ABORT = 4
} lti_lrresp_t;

/* Response Channel */

/* AHB3Lite Master Transaction */
typedef struct {
    uint64_t iova;
    uint32_t dev_id;
    uint32_t proc_id;
    lti_laflow_t flow_type;
    bool is_priv;
    bool is_write;
} LTI_LA_s;

/* AHB3Lite Slave Transaction */
typedef struct {
    lti_lrresp_t resp;
    uint64_t ppn;
    /* MRIF fields */
    uint64_t mrif_fields;
} LTI_LR_s;

/* ============= AMBA AXI4 Protocol ============= */
/* Maximum size of PTE fetched from memory (in bytes) */
#define DDT_NON_LEAF_PTE_BYTES  8
#define DDT_BASE_DC_PTE_BYTES   32
#define DDT_EXTD_DC_PTE_BYTES   64

#define PDT_NON_LEAF_PTE_BYTES  8
#define PDT_PC_PTE_BYTES        16

#define MAX_PTE_BYTES  DDT_EXTD_DC_PTE_BYTES

/* AXI4 Response Status */
typedef enum {
    OKAY,
    EXOKAY,
    SLVERR,
    DECERR
} axi4_access_t;

/* AXI4 Response */
typedef union {
    struct {
        uint8_t InD : 1;
        uint8_t NS  : 1; // always 1
        uint8_t PnU : 1;
    };
    uint8_t raw : 3;
} prot_u;

/* AXI4 Request */
typedef struct {
    uint64_t addr;

    /* InD | 1 | PnU */
    prot_u access_prot;
    bool is_write;

    size_t bytes;
    void *write_data;
} axi4_reqt_t;

typedef struct {
    axi4_access_t resp;

    size_t bytes;
    void *pte;
} axi4_resp_t;
