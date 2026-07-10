#include "qemu/osdep.h"
#include "exec/memattrs.h"
#include "system/dma.h"
#include "chardev/char.h"
#include "qemu/thread.h"
#include "trace.h"

#include "riscv_qemu_rtl_intf.h"

QemuCond lti_resp_wait_cond;
QemuMutex lti_resp_mutex;

void default_rtl_protocol_event_handler(void *opaque, QEMUChrEvent event, const char id_str[5])
{
    CharFrontend *fe = opaque;
    /* Upon OPEN, send id string to QRB server */
    switch (event) {
        case CHR_EVENT_OPENED:
            qemu_chr_fe_write_all(fe, (uint8_t *) id_str, 4);
            break;
        default:
            break;
    }
}

void ahb3lite_event_handler(void *opaque, QEMUChrEvent event)
{
    default_rtl_protocol_event_handler(opaque, event, "reqt");
}

MemTxResult rtl_mmio_rmw(hwaddr addr, bool is_write, bool is_8bytes,
                         uint64_t wdata, uint64_t *rdata,
                         CharFrontend *ahb_fe)
{
    int chardev_status;
    ahb3lite_master_reqt_s mtrans;
    ahb3lite_slave_resp_s strans;

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
    lti_args_s *lti_args = opaque;
    /* Upon OPEN, send id string to QRB server */
    switch (event) {
        case CHR_EVENT_OPENED:
            /* Initializing locks and mutexes */
            qemu_cond_init(&lti_resp_wait_cond);
            qemu_mutex_init(&lti_resp_mutex);
            /* Writing ID to LTI socket intf */
            qemu_chr_fe_write_all(&lti_args->lti_fe, (uint8_t *) "reqt", 4);
            break;
        default:
            break;
    }
    // default_rtl_protocol_event_handler(opaque, event, "reqt");
}

int can_read_lti_response(void *opaque)
{
    // Both request and response sizes are 24 bytes, see if we need to add a
    // check to see if we have request or response
    return sizeof(lti_LR_s);
}

void read_lti_response(void *opaque, const uint8_t *buf, int size)
{
    const char *resp_status;
    lti_args_s *lti_args = opaque;

    assert(size == sizeof(lti_LR_s));
    memcpy(&lti_args->lti_resp, buf, size);

    switch (lti_args->lti_resp.resp) {
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
    trace_qrb_lti_resp(resp_status, lti_args->lti_resp.ppn,
                       lti_args->lti_resp.mrif_fields,
                       (lti_args->lti_resp.mrif_fields >> LTI_LRUSER_NPPN_OFFSET) & LTI_LRUSER_NPPN_MASK,
                       lti_args->lti_resp.mrif_fields & LTI_LRUSER_NID_MASK);
    qemu_cond_broadcast(&lti_resp_wait_cond);
}

void rtl_lti_translate(hwaddr iova, bool is_write, bool is_priv,
                       uint32_t dev_id, bool proc_id_valid,
                       uint32_t proc_id, CharFrontend *lti_fe)
{
    lti_LA_s req;
    // lti_LR_s resp;
    // const char *resp_status;
    int chardev_status;

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
        // return 0;
        return;
    }

    /* Waiting for LTI response from QRB */
    // chardev_status = qemu_chr_fe_read_all(lti_fe, (uint8_t*)&resp, sizeof(resp));

    // if (chardev_status == -1) {
    //     return 0;
    // }
    // switch (resp.resp) {
    //     case LTI_RESP_SUCCESS:
    //         resp_status = "SUCCESS";
    //         break;
    //     case LTI_RESP_MRIF_SUCCESS:
    //         resp_status = "MRIF_SUCCESS";
    //         break;
    //     case LTI_RESP_FAULT_ABORT:
    //         resp_status = "FAULT_ABORT";
    //         break;
    //     default:
    //         resp_status = "INVALID_RESP";
    // }
    // trace_qrb_lti_resp(resp_status, resp.ppn, resp.mrif_fields,
    //                         (resp.mrif_fields >> LTI_LRUSER_NPPN_OFFSET) & LTI_LRUSER_NPPN_MASK,
    //                         resp.mrif_fields & LTI_LRUSER_NID_MASK);

    // /* Translation successful, updating iotlb data structure with translated address */
    // return resp.ppn;
}

/* AXI */
void axi4_event_handler(void *opaque, QEMUChrEvent event)
{
    default_rtl_protocol_event_handler(opaque, event, "resp");
}

int rtl_can_dram_access(void *opaque)
{
    return opaque ? sizeof(axi4_reqt_s) : 0;
}

void rtl_dram_access(void *opaque, const uint8_t *buf, int size)
{
    axi4_reqt_s reqt;
    axi4_resp_s resp;
    MemTxResult mem_status;

    CharFrontend *axi4_fe = opaque;
    memcpy(&reqt, buf, sizeof(reqt));

    /* Thread initially waits on RTL to send memory access */
    // bytes = qemu_chr_fe_read_all(axi4_fe, (uint8_t *)&reqt, sizeof(reqt));

    trace_qrb_axi4_reqt(reqt.addr,
                        (reqt.is_write ? "WRITE" : "READ"),
                        reqt.bytes);
    /* Performing required dma_memory_* function based on type of request */
    if (reqt.is_write) {
        mem_status = dma_memory_write(&address_space_memory, reqt.addr,
                                      reqt.write_data, reqt.bytes,
                                      MEMTXATTRS_UNSPECIFIED);
        /* Reponse PTE is all zeros if write operation */
        bzero(resp.pte, sizeof(resp.pte));
    } else {
        mem_status = dma_memory_read(&address_space_memory, reqt.addr,
                                     resp.pte, reqt.bytes,
                                     MEMTXATTRS_UNSPECIFIED);
        resp.bytes = mem_status == MEMTX_OK ? reqt.bytes : 0;
    }

    /** If operation successful, operation returns number of bytes read/written,
     * else it returns 0 */
    resp.resp = mem_status == MEMTX_OK ? OKAY : SLVERR;
    resp.bytes = mem_status == MEMTX_OK ? reqt.bytes : 0;

    /* Writing memory response to QRB */
    size = qemu_chr_fe_write_all(axi4_fe, (uint8_t *)&resp, sizeof(resp));
    /* Unable to write to socket, exit thread */
    if (size == 0) {
        return;
    }
    trace_qrb_axi4_resp(mem_status == MEMTX_OK ? "OKAY" : "SLVERR");
}
