#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/error-report.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/misc/riscv_msirem.h"
#include "hw/qdev-properties-system.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "exec/address-spaces.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "trace.h"

#define MSIREMAP_PTBR           0x000
#define PTBR_PPN_MASK           ((1UL << 44) - 1)
#define PTBR_PPN_SHIFT          8
#define PTBR_MODE_MASK          0xF
#define PTBR_MASK               ((PTBR_PPN_MASK << PTBR_PPN_SHIFT) | PTBR_MODE_MASK)

#define MSIREMAP_FLBR           0x008
#define FLBR_QSIZE_MASK         0x1F
#define FLBR_QSIZE_SHIFT        52
#define FLBR_PPN_MASK           PTBR_PPN_MASK
#define FLBR_PPN_SHIFT          PTBR_PPN_SHIFT
#define FLBR_MASK               (((uint64_t)FLBR_QSIZE_MASK << FLBR_QSIZE_SHIFT) | \
                                 ((uint64_t)FLBR_PPN_MASK << FLBR_PPN_SHIFT))

#define MSIREMAP_FLQC           0x010
#define FLQC_IRQEN              (1 << 3)
#define FLQC_CLR                (1 << 2)
#define FLQC_OFLOW_CLR          (1 << 1)
#define FLQC_EN                 0x1
#define FLQC_MASK               (FLQC_IRQEN | FLQC_EN)

#define MSIREMAP_FLHEAD         0x018
#define FLHEAD_MASK             ((1UL << 32) - 1)

#define MSIREMAP_FLTAIL         0x020
#define FLTAIL_MASK             ((1UL << 32) - 1)

#define MSIREMAP_STATUS         0x028
#define STATUS_OFLOW            (1 << 7)
#define STATUS_FAULT            (1 << 6)
#define STATUS_BUSY             (1 << 5)
#define STATUS_QFULL            (1 << 4)
#define STATUS_QEMPTY           (1 << 3)
#define STATUS_MASK             (STATUS_OFLOW | STATUS_FAULT | STATUS_BUSY | \
                                 STATUS_QFULL | STATUS_QEMPTY)

#define MSIREMAP_CTRL           0x030
#define CTRL_TEST_MODE          (1 << 7)
#define CTRL_PERF_RST           (1 << 6)
#define CTRL_FAULT_CLR          (1 << 5)
#define CTRL_SOFT_RST           (1 << 4)
#define CTRL_FAULT_IRQEN        (1 << 1)
#define CTRL_EN                 0x1
#define CTRL_MASK               (CTRL_TEST_MODE | CTRL_FAULT_CLR | \
                                 CTRL_FAULT_IRQEN | CTRL_EN)

#define MSIREMAP_IMSIC_BASE     0x038
#define IMSIC_BASE_MASK         ((1UL << 56) - 1)

#define MSIREMAP_IMSIC_STRIDE   0x040
#define IMSIC_STRIDE_MASK       ((1UL << 12) - 1)

#define MSIREMAP_IMSIC_PRIV_OFF 0x048
#define IMSIC_PRIV_OFF_MASK     ((1UL << 12) - 1)

#define MSIREMAP_FAULT_INJ      0x050
#define FAULT_INJ_CODE_MASK     ((1UL << 8) - 1)

#define MSIREMAP_PERF_CTR       0x058
#define PERF_CTR_COUNT_MASK     ((1UL << 32) - 1)

#define MSIREMAP_PERF_FAULT     0x060
#define PERF_FAULT_COUNT_MASK   ((1UL << 32) - 1)

#define MSIREMAP_LAST_MSI       0x068
#define LAST_MSI_DATA_MASK      ((1UL << 32) - 1)

#define MSIREMAP_VERSION        0x070
#define VERSION_MASK            ((1UL << 8) - 1)

#define MSIREMAP_COALESCE_NS    0x078

#define MSIREMAP_COALESCE_MAX   0x080
#define COALESCE_MAX_MASK       ((1UL << 16) - 1)

#define MSIREMAP_NOTIF_CTRL     0x088
#define NOTIF_CTRL_PWRDN_EN     0x2
#define NOTIF_CTRL_RESET_EN     0x1
#define NOTIF_CTRL_MASK         (NOTIF_CTRL_PWRDN_EN | NOTIF_CTRL_RESET_EN)

#define MSIREMAP_CHARDEV_CTRL   0x090
#define CHARDEV_CTRL_HEX_DUMP   (1 << 2)
#define CHARDEV_CTRL_VERBOSE    (1 << 1)
#define CHARDEV_CTRL_EN         0x1
#define CHARDEV_CTRL_MASK       (CHARDEV_CTRL_HEX_DUMP | CHARDEV_CTRL_VERBOSE | \
                                 CHARDEV_CTRL_EN)

#define MSIREMAP_TRACE_MASK     0x098
#define TRACE_MASK_BH           (1 << 5)
#define TRACE_MASK_COALESCE     (1 << 4)
#define TRACE_MASK_FAULT        (1 << 3)
#define TRACE_MASK_DELIVER      (1 << 2)
#define TRACE_MASK_PTE_FETCH    (1 << 1)
#define TRACE_MASK_MSI_RX       0x1
#define TRACE_MASK_ALL          (TRACE_MASK_BH | TRACE_MASK_COALESCE | \
                                 TRACE_MASK_FAULT | TRACE_MASK_DELIVER | \
                                 TRACE_MASK_PTE_FETCH | TRACE_MASK_MSI_RX)

#define MSIREMAP_BH_PENDING     0x0A0
#define BH_PENDING_COUNT_MASK   ((1UL << 8) - 1)

#define MSIREMAP_HOTPLUG_SEQ    0x0A8

#define MSIREMAP_DOORBELL       0xF00
#define DOORBELL_MSI_MASK       ((1UL << 32) - 1)

#define PTE_VALID               (1UL << 63)
#define PTE_LEAF                (1UL << 62)
#define PTE_PPN_MASK            ((1UL << 44) - 1)
#define PTE_PPN_SHIFT           18
#define PTE_HART_IDX_MASK       ((1UL << 8) - 1)
#define PTE_HART_IDX_SHIFT      54
#define PTE_PRIV_MASK           0x3
#define PTE_PRIV_SHIFT          52
#define PTE_GIDX_MASK           ((1UL << 8) - 1)
#define PTE_GIDX_SHIFT          44
#define PTE_EIID_MASK           ((1UL << 32) - 1)
#define PTE_EIID_SHIFT          12
#define PTE_NONLEAF_PRIV_MASK   ((1UL << 18) - 1)
#define PTE_LEAF_PRIV_MASK      ((1UL << 12) - 1)

#define MSI_INDEX_MASK          ((1UL << 8) - 1)

/**
 * Chardev Logger
 */

static void chardev_log_pte(RISCVMSIRemState *s, uint64_t pte)
{
    uint8_t hex_dump_msg[] = "PTE DUMP:\n";
    uint8_t char_buff[128];
    size_t len;
    qemu_chr_fe_write(&s->debug_logger, hex_dump_msg, sizeof(hex_dump_msg));

    if (s->chardev_ctrl & CHARDEV_CTRL_HEX_DUMP) {
        /* Converting the hex value to string and then storing it */
        len = snprintf((char *) char_buff, sizeof(char_buff),
                       "HEX: 0x%" PRIx64 "\n", pte);
        qemu_chr_fe_write(&s->debug_logger, char_buff, len);
    }

    if (s->chardev_ctrl & CHARDEV_CTRL_VERBOSE) {
        len = snprintf((char *) char_buff, sizeof(char_buff),
                       "Valid: %1d, Leaf: %1d, ",
                       !!(pte & PTE_VALID),
                       !!(pte & PTE_LEAF));
        qemu_chr_fe_write(&s->debug_logger, char_buff, len);
        if (pte & PTE_LEAF) {
            /* Verbose logging for leaf PPN */
            len = snprintf((char *) char_buff, sizeof(char_buff),
                           "HIDX: %" PRIu64 ", GIDX: %" PRIu64 ", Priv: %" PRIu32 ", EIID: 0x%" PRIx64 "\n",
                            pte >> PTE_HART_IDX_SHIFT & PTE_HART_IDX_MASK,
                            pte >> PTE_GIDX_SHIFT & PTE_GIDX_MASK,
                            (uint32_t) (pte >> PTE_PRIV_SHIFT & PTE_PRIV_MASK),
                            pte >> PTE_EIID_SHIFT & PTE_EIID_MASK);
            qemu_chr_fe_write(&s->debug_logger, char_buff, len);
        } else {
            /* Verbose logging for non-leaf PPN */
            len = snprintf((char *) char_buff, sizeof(char_buff),
                           "PPN: 0x%" PRIx64 "\n",
                           pte >> PTE_PPN_SHIFT & PTE_PPN_MASK);
            qemu_chr_fe_write(&s->debug_logger, char_buff, len);
        }
    }
}

static void chardev_log_fault(RISCVMSIRemState *s, uint32_t msi,
                              uint8_t fault_code)
{
    uint8_t hex_dump_msg[] = "FAULT DUMP: ";
    uint8_t fault_err[32];
    size_t len;
    qemu_chr_fe_write(&s->debug_logger, hex_dump_msg, sizeof(hex_dump_msg));

    len = snprintf((char *) fault_err, sizeof(fault_err), "CODE: 0x%" PRIx8 "\n", fault_code);
    qemu_chr_fe_write(&s->debug_logger, fault_err, len);

    if (s->chardev_ctrl & CHARDEV_CTRL_VERBOSE) {
        len = snprintf((char *) fault_err, sizeof(fault_err), "MSI: 0x%" PRIx32 "\n",
                       fault_code);
        qemu_chr_fe_write(&s->debug_logger, fault_err, len);
    }
}

/**
 * Fault subsystem
 */

static void fault_sb_to_DRAM(void *opaque)
{
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);
    MemTxResult res = MEMTX_OK;
    hwaddr addr;
    uint64_t qsize, qsize_mask, next_flhead;
    FaultLog *f;

    if (s->trace_mask & TRACE_MASK_BH) {
        trace_fault_sb_to_DRAM();
    }

    qsize = s->flbr >> FLBR_QSIZE_SHIFT & FLBR_QSIZE_MASK;
    qsize_mask = (1 << qsize) - 1;
    next_flhead = s->flhead + 1;

    if (((next_flhead + 1) & qsize_mask) != s->fltail) {
        /* RING Buffer is now full */
        s->status |= STATUS_QFULL;
    }

    /* Checking if there is a difference of 1 entry between next flhead and
     * current fltail. Otherwise DRAM ring buffer full, so not write to DRAM */
    if (!(s->status & STATUS_QFULL)) {
        addr = s->flbr >> FLBR_PPN_SHIFT & FLBR_PPN_MASK;
        addr <<= 12;
        addr += (hwaddr) (s->flhead << 4);
        f = g_queue_peek_head(s->staging_buffer);
        /* Saving the first double */
        address_space_stq_le(&address_space_memory, addr,
                             f->fault_info,
                             MEMTXATTRS_UNSPECIFIED, &res);
        if (res == MEMTX_OK) {
            /* Saving the second double */
            address_space_stq_le(&address_space_memory, addr + 8,
                                 f->timestamp_ns,
                                 MEMTXATTRS_UNSPECIFIED, &res);
            /* If all records save correctly, removing fault from stagging buffer */
            if (res == MEMTX_OK) {
                /* This means queue was initially empty */
                if (s->fltail == s->flhead) {
                    s->status &= ~STATUS_QEMPTY;
                }
                f = g_queue_pop_head(s->staging_buffer);
                g_free(f);
                s->flhead = next_flhead & qsize_mask;
                if (s->flqc & FLQC_IRQEN) {
                    qemu_irq_raise(s->fault_irq);
                }
            }
        }
    }

}

static void fault_subsystem_init(RISCVMSIRemState *s)
{
    s->staging_buffer = g_queue_new();
    /* Scheduling write from staging buffer -> DRAM to bottom half */
    s->staging_buffer_bh = qemu_bh_new(fault_sb_to_DRAM, s);
}

static void fault_logger(RISCVMSIRemState *s, uint32_t msi_data,
                         uint8_t fault_code)
{
    /* Saturating perf_fault counter */
    if (s->perf_fault < (1UL << 32)) {
        s->perf_fault++;
    }

    if (s->trace_mask & TRACE_MASK_FAULT) {
        trace_fault_logger(msi_data, fault_code);
    }
    if (s->chardev_ctrl & CHARDEV_CTRL_EN) {
        chardev_log_fault(s, msi_data, fault_code);
    }

    if (g_queue_get_length(s->staging_buffer) >= BH_PENDING_MAX) {
        /* GQueue overflow */
        s->status |= STATUS_OFLOW;
        return;
    }

    FaultLog *f = g_new(FaultLog, 1);
    f->fault_info = (uint64_t)msi_data << 32 | fault_code;
    f->timestamp_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    g_queue_push_tail(s->staging_buffer, f);

    /* Check if we need to schedule bottom half here */
}

/**
 * Translation Unit
 */

static void clear_busy(struct rcu_head *rp)
{
    RISCVMSIRemState *s = container_of(rp, RISCVMSIRemState, rcu);
    qemu_mutex_lock(&s->mutex);
    qatomic_rcu_set(&s->status, s->status & ~STATUS_BUSY);
    qemu_mutex_unlock(&s->mutex);
}

static void set_busy(struct rcu_head *rp)
{
    RISCVMSIRemState *s = container_of(rp, RISCVMSIRemState, rcu);
    qemu_mutex_lock(&s->mutex);
    qatomic_rcu_set(&s->status, s->status | STATUS_BUSY);
    qemu_mutex_unlock(&s->mutex);
}


static void riscv_msirem_send_msi(RISCVMSIRemState *s, hwaddr imsic_addr, uint32_t eiid)
{
    MemTxResult res;
    address_space_stq_le(&address_space_memory, imsic_addr,
                         eiid, MEMTXATTRS_UNSPECIFIED, &res);

    if (res != MEMTX_OK) {
        /* DMA access to IMSIC failed */
        fault_logger(s, eiid, FAULT_ACCESS_ERROR);
    }
    /* Translation Process finished */
    call_rcu1(&s->rcu, clear_busy);
}

/* IMSIC Delivery engine */
static void invoke_imsic_dengine(RISCVMSIRemState *s, uint32_t eiid,
                                 uint8_t hart_idx, uint8_t guest_idx,
                                 uint8_t priv)
{
    hwaddr imsic_addr;
    const char *priv_name = "Machine";

    if (priv > VSMODE) {
        fault_logger(s, eiid, FAULT_INVALID_PRIV);
        return;
    }

    imsic_addr = s->imsic_base & IMSIC_BASE_MASK;
    imsic_addr += (s->imsic_stride & IMSIC_STRIDE_MASK) * hart_idx;

    if (priv > MMODE) {
        priv_name = "Supervisor";
        imsic_addr += s->imsic_priv_off & IMSIC_PRIV_OFF_MASK;
    }
    if (priv > SMODE) {
        priv_name = "Virtual Supervisor";
        imsic_addr += (1 + guest_idx) * 0x1000;
    }

    if (s->trace_mask & TRACE_MASK_DELIVER) {
        trace_invoke_imsic_dengine(eiid, hart_idx, guest_idx, priv_name);
    }

    riscv_msirem_send_msi(s, imsic_addr, eiid);
}

static inline uint64_t get_pte(hwaddr addr, hwaddr offset, MemTxResult *res)
{
    return address_space_ldq_le(&address_space_memory, addr + (offset << 3),
                                MEMTXATTRS_UNSPECIFIED, res);
}

/* Page Table Walker */
static void pgtb_walker(RISCVMSIRemState *s, uint32_t msi, uint8_t walk_depth)
{
    MemTxResult res;
    uint64_t pte;
    hwaddr pgtb_base, addr;
    uint32_t eiid;
    uint8_t index, fault, priv, hart_idx, guest_idx;

    call_rcu1(&s->rcu, set_busy);
    /* RCU Guard while reading the pgtb & register ptbr */
    /* This is reptitive as all memory operations already define RCU Guards */
    WITH_RCU_READ_LOCK_GUARD() {
        /* Setting up PPN from root pgtb */
        pgtb_base = qatomic_rcu_read(&s->ptbr);
        pgtb_base = pgtb_base >> PTBR_PPN_SHIFT & PTBR_PPN_MASK;
        pgtb_base <<= 12;

        /* Index C is always index the final level pgtb */
        for (uint8_t i = walk_depth - 1; i <= 0; i--) {
            addr = address_space_ldq_le(&address_space_memory, pgtb_base,
                                        MEMTXATTRS_UNSPECIFIED, &res);
            if (res != MEMTX_OK) {
                /* Root page table is not configured */
                fault_logger(s, 0, FAULT_ACCESS_ERROR);
                return;
            }

            index = msi >> (i * 8) & MSI_INDEX_MASK;
            pte = get_pte(addr, index, &res);

            if (s->trace_mask & TRACE_MASK_PTE_FETCH) {
                trace_pgtb_walker((uint64_t) (addr + (index << 3)), pte);
            }

            if (s->chardev_ctrl & CHARDEV_CTRL_EN) {
                chardev_log_pte(s, pte);
            }

            if (res != MEMTX_OK) {
                /* PTE does not exist */
                fault = FAULT_ACCESS_ERROR;
                goto fault_exception;
                return;
            } else if (!(pte & PTE_VALID)) {
                fault = FAULT_INVALID_PTE;
                goto fault_exception;
            } else if (i < walk_depth - 1) {
                if (!(pte & PTE_LEAF)) {
                    fault = FAULT_UNEXPECTED_LEAF;
                    goto fault_exception;
                } else if ((pte & PTE_NONLEAF_PRIV_MASK) != 0) {
                    /* Found non-leaf pte's reserved bits to be 0 */
                    fault = FAULT_RESERVED_BITS;
                    goto fault_exception;
                }
            }
            pgtb_base = pte >> PTE_PPN_SHIFT & PTE_PPN_MASK;
            pgtb_base <<= 12;
            s->total_pgtb_walk++;
        }

        /* After walking the walk_depth we should expect to see a leaf pte */
        if ((pte & PTE_LEAF_PRIV_MASK) != 0) {
            fault = FAULT_RESERVED_BITS;
            goto fault_exception;
        } else if (!(pte & PTE_LEAF)) {
            fault = FAULT_EXPECTED_LEAF;
            goto fault_exception;
        }

        eiid = pte >> PTE_EIID_SHIFT & PTE_EIID_MASK;
        /* eiid 0 is invalid */
        if (eiid) {
            hart_idx = pte >> PTE_HART_IDX_SHIFT & PTE_HART_IDX_MASK;
            guest_idx = pte >> PTE_GIDX_SHIFT & PTE_GIDX_MASK;
            priv = pte >> PTE_PRIV_SHIFT & PTE_PRIV_MASK;
        }
    }

    if (eiid) {
        invoke_imsic_dengine(s, eiid, hart_idx, guest_idx, priv);
    }

    return;

fault_exception:
    fault_logger(s, 0, fault);
}

/* Translate Mode */
static void translate_msi(RISCVMSIRemState *s, uint32_t msi) {
    uint64_t mode = s->ptbr & PTBR_MODE_MASK;

    switch (mode) {
        case OFF:
            /* MSI discarded and fault is logged in stagging buffer */
            fault_logger(s, msi, FAULT_MODE_OFF);
            break;
        case BARE:
            invoke_imsic_dengine(s, msi, 0, 0, MMODE);
            break;
        case REMAP_1:
            pgtb_walker(s, msi, 1);
            break;
        case REMAP_2:
            pgtb_walker(s, msi, 2);
            break;
        case REMAP_3:
            pgtb_walker(s, msi, 3);
            break;
        default:
            /* Similar effect to OFF */
            fault_logger(s, msi, FAULT_MODE_OFF);
            break;
    }
}

/* Timer functions */
static void reset_cb_timer(RISCVMSIRemState *s)
{
    if (s->coalesce_ns > 0) {
        timer_mod_ns(&s->cb_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->coalesce_ns);
    }
}

static void cb_send_msi(void *opaque)
{
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);

    if (s->trace_mask & TRACE_MASK_COALESCE) {
        trace_cb_flush();
    }

    for (uint64_t i = 0; i < s->cb_count; i++) {
        translate_msi(s, s->cb[i]);
    }

    /* coalescing buffer is now cleared */
    s->cb_count = 0;
    reset_cb_timer(s);
}

/* Whenever there is a write to doorbell, it registers an msi for translation */
static void register_msi(RISCVMSIRemState *s)
{
    uint32_t msi = s->doorbell;

    s->last_msi = msi;

    /* Incrementing till saturation */
    if (s->perf_ctr < (1UL << 32)) {
        s->perf_ctr++;
    }

    if (s->trace_mask & TRACE_MASK_MSI_RX) {
        trace_register_msi(msi);
    }
    /* Put the msi in the coalescing buffer if it exists */
    if (s->coalesce_ns != 0 && s->coalesce_max > 1) {
        s->cb[s->cb_count++] = msi;

        if (s->trace_mask & TRACE_MASK_COALESCE) {
            trace_cb_enqueue(s->cb_count, s->coalesce_max);
        }

        /* Checking if the coalescing buffer if full
         * If it is full sending msi */
        if (s->cb_count == s->coalesce_max) {
            cb_send_msi(s);
        }
    } else {
        /* Otherwise start the translation of msi immediately upon its arrival */
        translate_msi(s, msi);
    }
}

/**
 * MMIO operations
 */

static uint64_t riscv_msirem_read(void *opaque, hwaddr addr, unsigned size)
{
    if ((addr & 0x7) != 0) {
        return 0;
    }

    RISCVMSIRemState *msirem = RISCV_MSIREM(opaque);

    switch (addr) {
        case MSIREMAP_PTBR:
            return msirem->ptbr;
        case MSIREMAP_FLBR:
            return msirem->flbr;
        case MSIREMAP_FLQC:
            return msirem->flqc;
        case MSIREMAP_FLHEAD:
            return msirem->flhead;
        case MSIREMAP_FLTAIL:
            return msirem->fltail;
        case MSIREMAP_STATUS:
            return msirem->status;
        case MSIREMAP_CTRL:
            return msirem->ctrl;
        case MSIREMAP_IMSIC_BASE:
            return msirem->imsic_base;
        case MSIREMAP_IMSIC_STRIDE:
            return msirem->imsic_stride;
        case MSIREMAP_IMSIC_PRIV_OFF:
            return msirem->imsic_priv_off;
        case MSIREMAP_PERF_CTR:
            return msirem->perf_ctr;
        case MSIREMAP_PERF_FAULT:
            return msirem->perf_fault;
        case MSIREMAP_LAST_MSI:
            return msirem->last_msi;
        case MSIREMAP_VERSION:
            return msirem->version;
        case MSIREMAP_COALESCE_NS:
            return msirem->coalesce_ns;
        case MSIREMAP_COALESCE_MAX:
            return msirem->coalesce_max;
        case MSIREMAP_NOTIF_CTRL:
            return msirem->notif_ctrl;
        case MSIREMAP_CHARDEV_CTRL:
            return msirem->chardev_ctrl;
        case MSIREMAP_TRACE_MASK:
            return msirem->trace_mask;
        case MSIREMAP_BH_PENDING:
            return g_queue_get_length(msirem->staging_buffer);
        case MSIREMAP_HOTPLUG_SEQ:
            return msirem->hotplug_seq;
    }

    /* If addr not not readable, return 0 */
    return 0;
}

static void riscv_msirem_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    if ((addr & 0x7) != 0) {
        return;
    }

    RISCVMSIRemState *msirem = RISCV_MSIREM(opaque);

    /* If addr not writable, ignore the write */
    switch (addr) {
        case MSIREMAP_PTBR:
            msirem->ptbr = data & PTBR_MASK;
            if (msirem->ctrl & CTRL_EN) {
                msirem->hotplug_seq++;
            }
            break;
        case MSIREMAP_FLBR:
            msirem->flbr = data & FLBR_MASK;
            break;
        case MSIREMAP_FLQC:
            msirem->flqc = data & FLQC_MASK;
            if (data & FLQC_CLR) {
                msirem->flhead = 0;
                msirem->fltail = 0;
                msirem->status |= STATUS_QEMPTY;
            }
            if (data & FLQC_OFLOW_CLR) {
                msirem->status &= ~STATUS_OFLOW;
            }
            break;
        case MSIREMAP_FLTAIL:
            msirem->fltail = data & FLTAIL_MASK;
            msirem->status &= ~STATUS_QFULL;
            if (msirem->fltail == msirem->flhead) {
                msirem->status |= STATUS_QEMPTY;
            }
            break;
        case MSIREMAP_CTRL:
            msirem->ctrl = data & CTRL_MASK;
            if (data & CTRL_PERF_RST) {
                msirem->perf_fault = 0;
                msirem->perf_ctr = 0;
            }
            if (data & CTRL_SOFT_RST) {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }
            if (data & CTRL_FAULT_IRQEN) {
                qemu_irq_raise(msirem->fault_irq);
            }
            break;
        case MSIREMAP_IMSIC_BASE:
            msirem->imsic_base = data & IMSIC_BASE_MASK;
            break;
        case MSIREMAP_IMSIC_STRIDE:
            msirem->imsic_stride = data & IMSIC_STRIDE_MASK;
            break;
        case MSIREMAP_IMSIC_PRIV_OFF:
            msirem->imsic_priv_off = data & IMSIC_PRIV_OFF_MASK;
            break;
        case MSIREMAP_FAULT_INJ:
            if (msirem->ctrl & CTRL_TEST_MODE) {
                msirem->fault_inj = data & FAULT_INJ_CODE_MASK;
            }
            break;
        case MSIREMAP_PERF_CTR:
            msirem->perf_ctr = data & PERF_CTR_COUNT_MASK;
            break;
        case MSIREMAP_PERF_FAULT:
            msirem->perf_fault = data & PERF_FAULT_COUNT_MASK;
            break;
        case MSIREMAP_COALESCE_NS:
            msirem->coalesce_ns = data;
            break;
        case MSIREMAP_COALESCE_MAX:
            msirem->coalesce_max = data & COALESCE_MAX_MASK;
            break;
        case MSIREMAP_NOTIF_CTRL:
            msirem->notif_ctrl = data & NOTIF_CTRL_MASK;
            if (msirem->notif_ctrl & NOTIF_CTRL_PWRDN_EN) {
                qemu_system_powerdown_request();
            } else if (msirem->notif_ctrl & NOTIF_CTRL_RESET_EN) {
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            }

            break;
        case MSIREMAP_CHARDEV_CTRL:
            msirem->chardev_ctrl = data & CHARDEV_CTRL_MASK;
            break;
        case MSIREMAP_TRACE_MASK:
            msirem->trace_mask = data & TRACE_MASK_ALL;
            break;
        case MSIREMAP_DOORBELL:
            if (msirem->ctrl & CTRL_EN) {
                msirem->doorbell = data & DOORBELL_MSI_MASK;
                register_msi(msirem);
            }
    }
}

static const MemoryRegionOps riscv_msirem_ops = {
    .read = riscv_msirem_read,
    .write = riscv_msirem_write,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8
    },
    .impl = {
        .min_access_size = 8,
        .max_access_size = 8
    }
};

/**
 * Runstate functions i.e. reset, powerdown, cleanup
 */
static void common_cleanup(RISCVMSIRemState *s)
{
    FaultLog *f;
    /* Flush the coalecing buffer */
    memset(&s->cb, 0, sizeof(uint64_t) * COALESCE_BUFF_MAX);

    /* Flush fault logs in staging buffer */
    while (!g_queue_is_empty(s->staging_buffer)) {
        f = g_queue_pop_head(s->staging_buffer);
        g_free(f);
    }

    /* Clearing bottom half */
    qemu_bh_cancel(s->staging_buffer_bh);
    /* Resetting timer */
    reset_cb_timer(s);
}

static void riscv_msirem_powerdown(Notifier *notifier, void *data)
{
    RISCVMSIRemState *s = container_of(notifier, RISCVMSIRemState,
                                       powerdown);
    /* Perform cleanup */
    common_cleanup(s);
    /* Write power-down marker in fault log pointed by flhead - 1 */
    fault_logger(s, 0, POWER_DOWN_MARKER);
}

static void riscv_msirem_reset(void *opaque)
{
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);
    /* Setting writable MMRs except FLBR and PTBR to 0x0 */
    s->flqc = 0;
    s->fltail = 0;
    s->ctrl = 0;
    s->imsic_base = 0;
    s->imsic_stride = 0;
    s->imsic_priv_off = 0;
    s->fault_inj = 0;
    s->perf_ctr = 0;
    s->perf_fault = 0;
    s->coalesce_ns = 0;
    s->coalesce_max = 64;
    s->notif_ctrl = 0;
    s->chardev_ctrl = 0;
    s->trace_mask = 0xf;
    s->hotplug_seq = 0;

    common_cleanup(s);
}

static void chrdev_logger_event(void *opaque, QEMUChrEvent event)
{
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);
    uint8_t end_msg[] = "START LOGGING END!";

    if (event == CHR_EVENT_OPENED) {
        qemu_chr_fe_write(&s->debug_logger, end_msg, sizeof(end_msg));
    }
}

/**
 * QOM
 */
static void riscv_msirem_realize(DeviceState *dev, Error **errp)
{
    RISCVMSIRemState *msirem = RISCV_MSIREM(dev);

    /**
     * Generating memory region
     *
     *  +--------------------------------------+ 0x1000
     *  |              regfile                 |
     *  +--------------------------------------+ 0x0900
     *  |            alias region              |
     *  |           (Size: 0x100)              |
     *  +--------------------------------------+ 0x0800
     *  |              regfile                 |
     *  +--------------------------------------+ 0x0000
     */

    memory_region_init(&msirem->mmio, OBJECT(dev), TYPE_RISCV_MSIREM ".mmio",
                       MSIREM_SIZE);
    memory_region_init_io(&msirem->regfile, OBJECT(dev), &riscv_msirem_ops,
                          msirem, TYPE_RISCV_MSIREM, MSIREM_SIZE);
    memory_region_init_alias(&msirem->alias, OBJECT(dev),
                             TYPE_RISCV_MSIREM ".alias",
                             &msirem->regfile, 0, MSIREM_ALIAS_SIZE);
    memory_region_add_subregion(&msirem->mmio, 0, &msirem->regfile);
    memory_region_add_subregion_overlap(&msirem->mmio, MSIREM_ALIAS_OFFSET,
                                        &msirem->alias, 1);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &msirem->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &msirem->fault_irq);

    qemu_chr_fe_set_handlers(&msirem->debug_logger, NULL, NULL, chrdev_logger_event,
                             NULL, msirem, NULL, true);
}

static void riscv_msirem_unrealize(DeviceState *dev)
{
    RISCVMSIRemState *s = RISCV_MSIREM(dev);
    common_cleanup(s);
    g_queue_free(s->staging_buffer);
    qemu_bh_delete(s->staging_buffer_bh);
}

static Property riscv_msirem_properties[] = {
    DEFINE_PROP_UINT64("coalescing_window_timeout", RISCVMSIRemState, coalesce_ns, 0),
    DEFINE_PROP_UINT64("coalescing_window_size", RISCVMSIRemState, coalesce_max, 64),
    DEFINE_PROP_CHR("debug-logger", RISCVMSIRemState, debug_logger),
    DEFINE_PROP_END_OF_LIST()
};

static VMStateDescription vmstate_riscv_msirem = {
    .name = TYPE_RISCV_MSIREM,
    .version_id = 0x10,
    .minimum_version_id = 0x10,
    .fields = (VMStateField[]) {
        VMSTATE_UINT64(ptbr, RISCVMSIRemState),
        VMSTATE_UINT64(flbr, RISCVMSIRemState),
        VMSTATE_UINT64(flqc, RISCVMSIRemState),
        VMSTATE_UINT64(flhead, RISCVMSIRemState),
        VMSTATE_UINT64(fltail, RISCVMSIRemState),
        VMSTATE_UINT64(status, RISCVMSIRemState),
        VMSTATE_UINT64(ctrl, RISCVMSIRemState),
        VMSTATE_UINT64(imsic_base, RISCVMSIRemState),
        VMSTATE_UINT64(imsic_stride, RISCVMSIRemState),
        VMSTATE_UINT64(imsic_priv_off, RISCVMSIRemState),
        VMSTATE_UINT64(fault_inj, RISCVMSIRemState),
        VMSTATE_UINT64(perf_ctr, RISCVMSIRemState),
        VMSTATE_UINT64(perf_fault, RISCVMSIRemState),
        VMSTATE_UINT64(last_msi, RISCVMSIRemState),
        VMSTATE_UINT64(version, RISCVMSIRemState),
        VMSTATE_UINT64(coalesce_ns, RISCVMSIRemState),
        VMSTATE_UINT64(coalesce_max, RISCVMSIRemState),
        VMSTATE_UINT64(notif_ctrl, RISCVMSIRemState),
        VMSTATE_UINT64(chardev_ctrl, RISCVMSIRemState),
        VMSTATE_UINT64(trace_mask, RISCVMSIRemState),
        VMSTATE_UINT64(doorbell, RISCVMSIRemState),
        VMSTATE_END_OF_LIST()
    }
};

static void riscv_msirem_instance_init (Object *obj)
{
    RISCVMSIRemState *s = RISCV_MSIREM(obj);

    s->version = 0x10;
    s->trace_mask = 0xf;

    timer_init_ns(&s->cb_timer, QEMU_CLOCK_VIRTUAL, cb_send_msi, s);

    fault_subsystem_init(s);

    s->powerdown.notify = riscv_msirem_powerdown;
    qemu_register_powerdown_notifier(&s->powerdown);
    qemu_register_reset(riscv_msirem_reset, s);
    /* To powerdown qemu_system_powerdown_request */
    /* To soft reset qemu_system_reset_request */
}

static void riscv_msirem_get_pgtb_count(Object *obj, Visitor *v,
                                        const char *name, void *opaque,
                                        Error **errp)
{
    RISCVMSIRemState *s = RISCV_MSIREM(obj);
    uint64_t value = s->total_pgtb_walk;
    visit_type_uint64(v, name, &value, errp);
}

static char *riscv_msirem_get_trans_mode(Object *obj, Error **errp)
{
    RISCVMSIRemState *s = RISCV_MSIREM(obj);
    uint8_t value = (uint8_t) (s->ptbr & PTBR_MODE_MASK);
    const char *val;

    switch (value) {
        case OFF:
            val = "OFF";
            break;
        case BARE:
            val = "BARE";
            break;
        case REMAP_1:
            val = "REMAP-1";
            break;
        case REMAP_2:
            val = "REMAP-2";
            break;
        case REMAP_3:
            val = "REMAP-3";
            break;
        default:
            val = "Reserved (Defaulting to Bare)";
    }

    return g_strdup(val);
}

static void riscv_msirem_class_init (ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);

    dc->realize = riscv_msirem_realize;
    dc->desc = "MSI Remapper, performs MSI translation via page tables";
    dc->unrealize = riscv_msirem_unrealize;
    dc->vmsd = &vmstate_riscv_msirem;
    device_class_set_props(dc, riscv_msirem_properties);

    /* Dynamic properties, these are read only properties for now.
     * By introducing a setter callback, we can also set these properties
     * but that not make sense for these properties */
    object_class_property_add(class, "walk-count", "uint64_t",
                              riscv_msirem_get_pgtb_count,
                              NULL, NULL, NULL);
    object_class_property_add_str(class, "mode-name",
                                  riscv_msirem_get_trans_mode, NULL);
}

static const TypeInfo riscv_msirem_info = {
    .name = TYPE_RISCV_MSIREM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCVMSIRemState),
    .instance_init = riscv_msirem_instance_init,
    .class_init = riscv_msirem_class_init
};

static void riscv_msirem_register_type(void)
{
    type_register_static(&riscv_msirem_info);
}

type_init(riscv_msirem_register_type);

DeviceState *riscv_msirem_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_RISCV_MSIREM);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    return dev;
}
