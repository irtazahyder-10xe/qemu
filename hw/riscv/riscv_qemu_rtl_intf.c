#include "qemu/osdep.h"
#include "exec/memattrs.h"
#include "system/dma.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "hw/misc/edu.h"
#include "trace.h"

#include "riscv_qemu_rtl_intf.h"

struct {
    QemuSemaphore rtl_qemu_sem;
    ahb3lite_slave_resp_s resp;
} ahb_resp;

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

int can_read_rtl_mmio_resp(void *opaque)
{
    return sizeof(ahb3lite_slave_resp_s);
}

void rtl_mmio_read_resp(void *opaque, const uint8_t *buf, int size)
{
    assert(size == sizeof(ahb3lite_slave_resp_s));
    memmove(&ahb_resp.resp, buf, size);
    qemu_sem_post(&ahb_resp.rtl_qemu_sem);
}

MemTxResult rtl_mmio_rmw(hwaddr addr, bool is_write, bool is_8bytes,
                         uint64_t wdata, uint64_t *rdata,
                         CharFrontend *ahb_fe)
{
    int chardev_status;
    ahb3lite_master_reqt_s mtrans;
    ahb3lite_slave_resp_s strans;

    qemu_sem_init(&ahb_resp.rtl_qemu_sem, 0);

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

    bql_unlock();
    qemu_sem_wait(&ahb_resp.rtl_qemu_sem);
    bql_lock();
    memcpy(&strans, &ahb_resp.resp, sizeof(strans));

    qemu_sem_destroy(&ahb_resp.rtl_qemu_sem);
    /* Wait for AXI4 to also complete if register was debug control register */
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

    /**
     * If we are writing tr_req_ctl.Go/Busy, then do not issue another AHB
     * request before all AXI4 transactions for reference model are completed.
     */
    return MEMTX_OK;
}

#define LTI_ID_DEVICE_ID_BITS 8
#define LTI_ID_DEVICE_ID_MASK ((1 << LTI_ID_DEVICE_ID_BITS) - 1)

/*
 * GHashTable to store device id's and their corresponding EDUState
 * This helps us redirect IOMMU responses to required RTL
 */
GHashTable *edu_table = NULL;

bool insert_edu_dev_state(uint64_t dev_id, Object *obj)
{
    if (edu_table == NULL) {
        edu_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (obj == NULL) {
        return false;
    }
    trace_qrb_reg_edu(dev_id, (uintptr_t) EDU(obj));
    return g_hash_table_insert(edu_table, GUINT_TO_POINTER(dev_id), EDU(obj));
}

bool remove_edu_dev_state(uint64_t dev_id)
{
    return g_hash_table_remove(edu_table, GUINT_TO_POINTER(dev_id));
}

void lti_event_handler(void *opaque, QEMUChrEvent event)
{
    /* Only 256 devices supported */
    static uint8_t __ref = 0;
    EduState *edu = opaque;
    /* Upon OPEN, send id string to QRB server */
    switch (event) {
        case CHR_EVENT_OPENED:
            if (!__ref++) {
                /* Writing ID to LTI socket intf */
                trace_qrb_reg_edu(edu->pdev.devfn, (uintptr_t) edu);
                qemu_chr_fe_write_all(&edu->lti_fe, (uint8_t *) "reqt", 4);
            }
            break;
        default:
            break;
    }
    // default_rtl_protocol_event_handler(opaque, event, "reqt");
}

int can_read_rtl_trans_resp(void *opaque)
{
    return sizeof(lti_LR_s);
}

void read_rtl_trans_resp(void *opaque, const uint8_t *buf, int size)
{
    static uint8_t __buff[0x80];
    static uint8_t __buff_size;
    const char *resp_status;
    lti_LR_s resp;
    EduState *edu;

    if (size != sizeof(lti_LR_s))
    {
        memcpy(__buff + __buff_size, buf, size);
        __buff_size += size;
        return;
    }
    memcpy(&resp, buf, size);

    switch (resp.resp) {
        case LTI_RESP_SUCCESS:
            resp_status = "SUCCESS";
            break;
        case LTI_RESP_FAULT_ABORT:
            resp_status = "FAULT_ABORT";
            break;
        default:
            if (resp.mrif_fields & (1ULL << 59)) {
                resp_status = "MRIF_SUCCESS";
            } else {
                resp_status = "INVALID_RESP";
            }
    }
    /* Dispatching response to desired edu device */
    uint64_t dev_id = resp.id & LTI_ID_DEVICE_ID_MASK;
    edu = g_hash_table_lookup(edu_table, GUINT_TO_POINTER(dev_id));

    trace_qrb_lti_resp(resp.id, resp_status, resp.spa,
                       resp.mrif_fields,
                       (resp.mrif_fields >> LTI_LRUSER_NPPN_OFFSET) & LTI_LRUSER_NPPN_MASK,
                       resp.mrif_fields & LTI_LRUSER_NID_MASK);

    edu_perform_dma(edu, resp);
}

uint64_t rtl_trans_reqt(hwaddr iova, bool is_write, bool is_priv,
                        uint32_t dev_id, bool proc_id_valid,
                        uint32_t proc_id, CharFrontend *lti_fe)
{
    /* Static ID assigned to every function caller to differentiate between
     * responses */
    static uint64_t lti_id = 0;
    lti_LA_s req;

    req.id = (lti_id++ << LTI_ID_DEVICE_ID_BITS) | dev_id;
    req.iova = iova;
    req.dev_id = dev_id;
    req.is_proc_valid = proc_id_valid;
    req.proc_id = proc_id;
    req.flow_type = LTI_FLOW_NO_STALL;
    req.is_priv = is_priv;
    req.is_write = is_write;

    /* Breker has no ATS tests so flow always NO_STALL */
    trace_qrb_lti_reqt(req.id, req.iova, req.dev_id, req.is_proc_valid,
                       req.proc_id, "NO_STALL", req.is_priv, req.is_write);

    /* Sending LTI request to QRB */
    /* TODO: Check for write fails */
    qemu_chr_fe_write_all(lti_fe, (uint8_t *)&req, sizeof(req));
    return req.id;
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

    trace_qrb_axi4_reqt(reqt.id, reqt.addr,
                        (reqt.is_write ? "WRITE" : "READ"),
                        reqt.bytes);
    /* Performing required dma_memory_* function based on type of request */
    mem_status = dma_memory_read(&address_space_memory, reqt.addr,
                                 resp.pte, reqt.bytes,
                                 MEMTXATTRS_UNSPECIFIED);
    if (reqt.is_write && mem_status == MEMTX_OK) {
        /* Apply strobe mask on PTE */
        for (uint8_t i = 0; i < reqt.bytes; i++) {
            if (reqt.bytes < 8 && reqt.write_strb[0] == ((1 << reqt.bytes) - 1)) {
                /* Optimization if we have a write of less than a byte
                 * and strobe has 1's in [0:bytes-1] */
                break;
            } else if (reqt.write_strb[i] == 0xFF) {
                /* Optimization to skip processing double words if byte
                 * contains all 1s */
                i += 7;
                continue;
            }

            /**
             * Checking strobe bit for ith byte
             * If it is zero, keeping byte same as it was originally
             * stored in memory
             */
            if (!(reqt.write_strb[i >> 3] & (1 << (i % 8)))) {
                reqt.write_data[i] = resp.pte[i];
            }
        }
        mem_status = dma_memory_write(&address_space_memory, reqt.addr,
                                      reqt.write_data, reqt.bytes,
                                      MEMTXATTRS_UNSPECIFIED);
        /* Reponse PTE is all zeros if write operation */
        bzero(resp.pte, sizeof(resp.pte));
    }

    /* ID same as request, ID is used by RTL so returning it as it is */
    resp.id = reqt.id;
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
    trace_qrb_axi4_resp(resp.id, mem_status == MEMTX_OK ? "OKAY" : "SLVERR");
}

void rv_intf_cleanup(void)
{
    g_hash_table_destroy(edu_table);
}
