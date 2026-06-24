#include "qemu/osdep.h"
#include "exec/memattrs.h"
#include "chardev/char.h"
#include "riscv-iommu.h"
#include "trace.h"

#include "riscv_qemu_rtl_intf.h"

void ahb3lite_event_handler(void *opaque, QEMUChrEvent event)
{
    RISCVIOMMUState *s = RISCV_IOMMU(opaque);
    /* Upon OPEN, send reqt to server to register QEMU AHB requestor */
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(&s->ahb3lite_fe, (uint8_t *) "reqt", 4);
            break;
        default:
            break;
    }
}

MemTxResult rtl_mmio_rmw(hwaddr addr, bool is_write, bool is_8bytes,
                         uint64_t wdata, uint64_t *rdata,
                         CharFrontend *ahb_fe)
{
    int chardev_status;
    ahb3lite_master_s mtrans;
    ahb3lite_slave_s strans;

    if (!qemu_chr_fe_backend_open(ahb_fe)) {
        /* Charbackend is not open */
        return MEMTX_ERROR;
    }

    mtrans.hwrite = is_write;
    mtrans.hsize = is_8bytes ? 3 : 2;

    mtrans.haddr = addr;
    mtrans.hwdata = wdata;

    /* Writing AHB request to QRB */
    chardev_status = qemu_chr_fe_write_all(ahb_fe, (uint8_t *)&mtrans,
                                           sizeof(mtrans));
    trace_qrb_ahb3lite_master(mtrans.hwdata,
                              mtrans.haddr,
                              mtrans.hsize,
                              mtrans.hwrite
                              );

    if (chardev_status == -1) {
        /* Unable to write to socket */
        return MEMTX_ERROR;
    }

    /* Waiting for AHB response from QRB */
    chardev_status = qemu_chr_fe_read_all(ahb_fe, (uint8_t*)&strans,
                                          sizeof(strans));
    if (chardev_status < 0) {
        return MEMTX_ERROR;
    }
    trace_qrb_ahb3lite_slave(strans.hrdata,
                             strans.hresp);

    /* If HRESP_ERROR, return MEMTX_ERROR */
    if (strans.hresp == AHB3L_HRESP_ERROR) {
        return MEMTX_ERROR;
    }

    /* If write request, write AHB3LITE.HRDATA to rdata */
    if (rdata && !is_write) {
        *rdata = strans.hrdata;
    }

    return MEMTX_OK;
}

void lti_event_handler(void *opaque, QEMUChrEvent event)
{
    RISCVIOMMUState *s = RISCV_IOMMU(opaque);
    /* Upon OPEN, send reqt to server to register QEMU LTI requestor */
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(&s->lti_fe, (uint8_t *) "reqt", 4);
            break;
        default:
            break;
    }
}

hwaddr rtl_lti_translate(hwaddr iova, bool is_write, bool is_priv,
                         uint32_t dev_id, bool proc_id_valid,
                         uint32_t proc_id, CharFrontend *lti_fe)
{
    lti_LA_s req;
    lti_LR_s resp;
    int chardev_status;
    const char *resp_status;

    req.iova = iova;
    req.dev_id = dev_id;
    req.is_proc_valid = proc_id_valid;
    req.proc_id = proc_id;
    req.flow_type = LTI_FLOW_NO_STALL;
    req.is_priv = is_priv;
    req.is_write = is_write;

    /* TODO: Add appropriate error handling
     * TODO: Update hard coded string when ATST flow supported */
    trace_qrb_lti_reqt(req.iova, req.dev_id, req.is_proc_valid, req.proc_id,
                       "NO_STALL", req.is_priv, req.is_write);

    /* Sending LTI request to QRB */
    chardev_status = qemu_chr_fe_write_all(lti_fe, (uint8_t *)&req, sizeof(req));
    if (chardev_status == -1) {
        return 0;
    }

    /* Waiting for LTI response from QRB */
    chardev_status = qemu_chr_fe_read_all(lti_fe, (uint8_t*)&resp, sizeof(resp));
    if (chardev_status == -1) {
        return 0;
    }
    switch (resp.resp) {
        case LTI_RESP_SUCCESS:
            resp_status = "SUCCESS";
            break;
        case LTI_RESP_MRIF_SUCCESS:
            resp_status = "MRIF_SUCCESS";
            break;
        case LTI_RESP_FAULT_ABORT:
            resp_status = "FAULT_ABORT";
            break;
        default:
            resp_status = "INVALID_RESP";
    }
    trace_qrb_lti_resp(resp_status, resp.ppn, resp.mrif_fields,
                            (resp.mrif_fields >> LTI_LRUSER_NPPN_OFFSET) & LTI_LRUSER_NPPN_MASK,
                            resp.mrif_fields & LTI_LRUSER_NID_MASK);

    /* Translation successful, updating iotlb data structure with translated address */
    return resp.ppn;
}

/* AXI */
static void axi4_event_handler(void *opaque, QEMUChrEvent event)
{
    CharFrontend *fe = opaque;
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(fe, (uint8_t *) "resp", 4);
            break;
        default:
            break;
    }
}

void *rtl_dram_access(void *args)
{
    axi4_reqt_t reqt;
    axi4_resp_t resp;
    ssize_t bytes;
    MemTxResult mem_status;
    CharFrontend axi4_fe;

    axi4_th_args_s *_args = args;
    Error *errp;
    /* Initializing chardev frontend */
    qemu_chr_fe_init(&axi4_fe, _args->axi4_chardev, &errp);
    qemu_chr_fe_set_handlers(&axi4_fe, NULL, NULL, axi4_event_handler,
                             NULL, &axi4_fe, NULL, true);

    while (true) {
        /* Thread initially waits on RTL to send memory access */
        bytes = qemu_chr_fe_read_all(&axi4_fe, (uint8_t *)&reqt, sizeof(reqt));

        /* Unable to read socket, exit thread */
        if (bytes <= 0) {
            break;
        }

        trace_qrb_axi4_reqt(reqt.addr,
                            (reqt.is_write ? "WRITE" : "READ"),
                            reqt.bytes);
        /* Performing required dma_memory_* function based on type of request */
        if (reqt.is_write) {
            mem_status = dma_memory_write(_args->as, reqt.addr,
                                          reqt.write_data, reqt.bytes,
                                          MEMTXATTRS_UNSPECIFIED);
            /* Reponse PTE is all zeros if write operation */
            bzero(resp.pte, sizeof(resp.pte));
        } else {
            mem_status = dma_memory_read(_args->as, reqt.addr,
                                         resp.pte, reqt.bytes,
                                         MEMTXATTRS_UNSPECIFIED);
            resp.bytes = mem_status == MEMTX_OK ? reqt.bytes : 0;
        }

        /** If operation successful, operation returns number of bytes read/written,
         * else it returns 0 */
        resp.resp = mem_status == MEMTX_OK ? AXI4_OKAY : AXI4_SLVERR;
        resp.bytes = mem_status == MEMTX_OK ? reqt.bytes : 0;

        /* Writing memory response to QRB */
        bytes = qemu_chr_fe_write_all(&axi4_fe, (uint8_t *)&resp, sizeof(resp));
        /* Unable to write to socket, exit thread */
        if (bytes <= 0) {
            break;
        }
        trace_qrb_axi4_resp(mem_status == MEMTX_OK ? "OKAY" : "SLVERR");
    }
    return NULL;
}
