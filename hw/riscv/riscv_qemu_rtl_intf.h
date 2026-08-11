#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "chardev/char-fe.h"
#include "exec/memattrs.h"
#include "exec/hwaddr.h"

/**
 * @brief Default event handler for sockets used to communicate with RTL
 *
 * This function is the callback for qemu_chr_fe_set_handlers for chardev frontends.
 *
 * @param opaque Pointer to the used CharFrontend
 * @param event  Event due to which callback triggered
 * @param id_str 4 byte id string to send QRB, last character is always NULL terminator.
 */
void default_rtl_protocol_event_handler(void *opaque, QEMUChrEvent event, const char id_str[5]);

/* ============= AMBA 3 ABH-Lite Protocol ============= */

/* AHB-Lite 3 Protocol Macros */
/* HWRITE: Transfer Direction */
#define AHB3L_HWRITE_READ       0x0U
#define AHB3L_HWRITE_WRITE      0x1U
/* HRESP: Response Type */
#define AHB3L_HRESP_OKAY        0x0U
#define AHB3L_HRESP_ERROR       0x1U

#define DOUBLE_ACCESS(size) (size == 8)

/* AHB3Lite Master Transaction */
typedef struct {
    bool hwrite;
    uint8_t hsize; /* Only 4 or 8 byte access allowed */

    uint32_t haddr;
    uint64_t hwdata;
} ahb3lite_master_reqt_s;

/* AHB3Lite Slave Transaction */
typedef struct {
    bool hresp;

    uint64_t hrdata;
} ahb3lite_slave_resp_s;

/**
 * @brief Event handler for AHB socket device events
 *
 * This function is the callback for qemu_chr_fe_set_handlers for AHB frontend.
 * QEMU is master in AHB protocl, hence it sends the reqt (requestor) id to QRB
 * on AHB_SOCK.
 *
 * @param opaque Pointer to be casted to RISCV_IOMMU. Used to fetch frontend
 * @param event  Event due to which callback triggered
 */
void ahb3lite_event_handler(void *opaque, QEMUChrEvent event);

/**
 * @brief QEMU -> RTL MMR read, modify, write bypass.
 *
 * Forwards any IOMMU MMR RMW request to serial port ahb_fe. The backend
 * is preferrably a unix socket forwarding AHB request to RTL.
 *
 * This function is to be called in riscv-iommu.c MemoryOps read and write
 * callbacks.
 *
 * @param addr      IOMMU MMR offset
 * @param is_write  Operation to be performed on MMR (Write or Read)
 * @param is_8bytes 8 byte of 4 byte access
 * @param wdata     Write data send to RTL MMR
 * @param rdata     If @is_write == false, contains data read from MMR
 * @param ahb_fe    Pointer to objects (type RISCV_IOMMU) frontend attribute
 *
 * NOTE: User is responsible for initializing varibale to hold rdata.
 *       For write operations @rdata is NULL.
 */
MemTxResult rtl_mmio_rmw(hwaddr addr, bool is_write, bool is_8bytes,
                         uint64_t wdata, uint64_t *rdata,
                         CharFrontend *ahb_fe);

/* ============= AMBA LTI A Protocol ============= */
/**
 * @name Request Channel Signals
 *
 * LTI_LRUSER: {NPPN[43:0],NID[10:0]}
 * @{
 */
#define LTI_LASID_DEVID_MASK    0xFFFFFFUL
#define LTI_LASSID_PROCID_MASK  0xFFFFFUL
/** @} */

/**
 * @name Response Channel Signals
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
    LTI_RESP_FAULT_ABORT = 4
} lti_lrresp_t;

/* LTI Request Channel Transaction */
typedef struct {
    uint64_t id;
    uint64_t iova;
    uint32_t dev_id;
    uint32_t proc_id;
    lti_laflow_t flow_type;

    bool is_proc_valid;
    bool is_priv;
    bool is_write;
} lti_LA_s;

#define MRIF_NID_MASK       ((1UL << 10) - 1)
#define MRIF_NPPN_MASK      ((1UL << 44) - 1)
#define MRIF_NPPN_OFFSET    11
#define MRIF_ADDR_MASK      ((1UL << 4) - 1)
#define MRIF_ADDR_OFFSET    55
#define MRIF_VALID          (1UL << 59)
/* LTI Response Channel Transaction */
typedef struct {
    uint64_t id;
    uint64_t spa;
    lti_lrresp_t resp;
    /* MRIF fields
     * NID: [10:0]
     * NPPN: [54:11]
     * MRIF Address: mrif_fields[58:55] concat spa
     * isMRIF: [59]
     */
    uint64_t mrif_fields; // 60 bits field
    /* QoS */
    uint16_t rcid; // 12 bit fields
    uint16_t mcid; // 12 bit fields
} lti_LR_s;

/**
 * @brief Event handler for LTI socket device events
 *
 * This function is the callback for qemu_chr_fe_set_handlers for LTI frontend.
 * QEMU is master in LTI protocl, hence it sends the reqt (requestor) id to QRB
 * on AHB_SOCK.
 *
 * @param opaque Pointer to be casted to EDU device.
 * @param event  Event due to which callback triggered
 * @param buf    Buffer with read data
 * @param size   Size of @buffer in bytes
 */

void lti_event_handler(void *opaque, QEMUChrEvent event);

int can_read_rtl_trans_resp(void *opaque);

void read_rtl_trans_resp(void *opaque, const uint8_t *buf, int size);

/**
 * @brief QEMU -> RTL IOVA translation request
 *
 * Forwards any IOMMU translation requests to serial port lti_fe. The backend
 * is preferrably a unix socket forwarding LTI request to RTL.
 *
 * @param iova          IO Virtual Address to be translated
 * @param is_write      Memory read or write operation on address @iova
 * @param is_priv       Priviledged or Unpriviledged access.
 * @param dev_id        Device ID
 * @param proc_id_valid Validates if @proc_id is valid or garbage
 * @param proc_id       Process ID
 * @param lti_fe        Pointer to objects (type RISCV_IOMMU) frontend attribute
 *
 * @return translation request id. This would be used to determine the response for DMA.
 * TODO: For now it is assumed the frontend is present in RISCV IOMMU. This
 *       is subjected to change and the frontend would be initialized in
 *       IO DEVICE.
 */
uint64_t rtl_trans_reqt(hwaddr iova, bool is_write, bool is_priv,
                        uint32_t dev_id, bool proc_id_valid,
                        uint32_t proc_id, CharFrontend *lti_fe);

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

/* Permissions */
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
    uint64_t id;    // Used by RTL
    uint64_t addr;

    /* InD | 1 | PnU */
    prot_u access_prot;
    bool is_write;

    size_t bytes;
    uint8_t write_data[MAX_PTE_BYTES];
    /* For every byte in write_data, we require a strobe value */
    uint8_t write_strb[MAX_PTE_BYTES >> 3];
} axi4_reqt_s;

/* AXI4 Response */
typedef struct {
    uint64_t id;    // Used by RTL
    axi4_access_t resp;

    size_t bytes;
    uint8_t pte[MAX_PTE_BYTES];
} axi4_resp_s;

void axi4_event_handler(void *opaque, QEMUChrEvent event);
int rtl_can_dram_access(void *opaque);
void rtl_dram_access(void *opaque, const uint8_t *buf, int size);
