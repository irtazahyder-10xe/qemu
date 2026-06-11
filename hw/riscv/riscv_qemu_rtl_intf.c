#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "trace.h"
#include "riscv-iommu.h"

#include "riscv_qemu_rtl_intf.h"

void ahb3lite_event_handler(void *opaque, QEMUChrEvent event)
{
    RISCVIOMMUState *s = RISCV_IOMMU(opaque);
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(&s->ahb3lite_fe, (uint8_t *) "reqt", 4);
            break;
        default:
            break;
    }
}

MemTxResult rtl_mmio_rmw(hwaddr addr, bool is_write, bool is_double,
                         uint64_t wdata, uint64_t *rdata,
                         CharFrontend *ahb_fe)
{
    int chardev_status;
    ahb3lite_mtrans_s mtrans;
    ahb3lite_strans_s strans;

    if (!qemu_chr_fe_backend_open(ahb_fe)) {
        /* Charbackend is not open */
        return MEMTX_ERROR;
    }

    mtrans.ahb3lite_hwrite = is_write;
    mtrans.ahb3lite_hready = true;
    mtrans.ahb3lite_hmastlock = false;
    mtrans.ahb3lite_hsize = is_double ? AHB3L_HSIZE_64BIT :
                                        AHB3L_HSIZE_32BIT;
    mtrans.ahb3lite_hburst = AHB3L_HBURST_SINGLE; /* SINGLE */
    mtrans.ahb3lite_htrans = AHB3L_HTRANS_NONSEQ; /* 2 => NONSEQ, 3 => SEQ */
    mtrans.ahb3lite_hprot = AHB3L_HPROT_DEFAULT; /* Privileged + Data access */
    mtrans.ahb3lite_haddr = addr;
    mtrans.ahb3lite_hwdata = wdata;

    chardev_status = qemu_chr_fe_write_all(ahb_fe, (uint8_t *)&mtrans,
                                           MASTER_TRANS_BYTES);
    trace_qemu2rtl_ahb3lite_master(mtrans.ahb3lite_hwdata,
                                   mtrans.ahb3lite_haddr,
                                   mtrans.ahb3lite_hsize,
                                   mtrans.ahb3lite_hburst,
                                   mtrans.ahb3lite_hprot,
                                   mtrans.ahb3lite_htrans,
                                   mtrans.ahb3lite_hwrite,
                                   mtrans.ahb3lite_hmastlock,
                                   mtrans.ahb3lite_hready);
    if (chardev_status == -1) {
        /* Unable to write to socket */
        return MEMTX_ERROR;
    }

    chardev_status = qemu_chr_fe_read_all(ahb_fe, (uint8_t*)&strans,
                                          SLAVE_TRANS_BYTES);
    if (chardev_status < 0) {
        return MEMTX_ERROR;
    }
    trace_qemu2rtl_ahb3lite_slave(strans.ahb3lite_hrdata,
                                  strans.ahb3lite_hresp,
                                  strans.ahb3lite_hreadyout);

    if (strans.ahb3lite_hresp == AHB3L_HRESP_ERROR) {
        return MEMTX_ERROR;
    }

    if (rdata && !is_write) {
        *rdata = strans.ahb3lite_hrdata;
    }

    return MEMTX_OK;
}

void lti_event_handler(void *opaque, QEMUChrEvent event)
{
    RISCVIOMMUState *s = RISCV_IOMMU(opaque);
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(&s->lti_fe, (uint8_t *) "reqt", 4);
            break;
        default:
            break;
    }
}

hwaddr rtl_lti_translate(hwaddr iova, bool is_write, bool is_priv,
                         uint32_t dev_id, uint32_t proc_id,
                         CharFrontend *lti_fe)
{
    LTI_LA_s req;
    LTI_LR_s resp;
    int chardev_status;
    const char *resp_status;

    req.iova = iova;
    req.dev_id = dev_id;
    req.proc_id = proc_id;
    req.flow_type = LTI_FLOW_NO_STALL;
    req.is_priv = is_priv;
    req.is_write = is_write;

    /* TODO: Add appropriate error handling
     * TODO: Update hard coded string when ATST flow supported */
    trace_qemu2rtl_lti_req(req.iova, req.dev_id, req.proc_id, "NO_STALL",
                           req.is_priv, req.is_write);

    chardev_status = qemu_chr_fe_write_all(lti_fe, (uint8_t *)&req, sizeof(req));
    if (chardev_status == -1) {
        return 0;
    }

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
    trace_qemu2rtl_lti_resp(resp_status, resp.ppn, resp.mrif_fields,
                            (resp.mrif_fields >> LTI_LRUSER_NPPN_OFFSET) & LTI_LRUSER_NPPN_MASK,
                            resp.mrif_fields & LTI_LRUSER_NID_MASK);

    /* Translation successful, updating iotlb data structure with translated address */
    return resp.ppn;
}

/* AXI */
