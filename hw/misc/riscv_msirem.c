#include "qemu/osdep.h"
#include "qemu/log.h"
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
#define PTBR_PPN_MASK           ((1ULL << (51 - 8 + 1)) - 1)
#define PTBR_PPN_SHIFT          8
#define PTBR_MDOE_MASK          0xF

#define MSIREMAP_FLBR           0x008
#define FLBR_QSIZE_MASK         0x1F
#define FLBR_QSIZE_SHIFT        52
#define FLBR_PPN_MASK           PTBR_PPN_MASK
#define FLBR_PPN_SHIFT          PTBR_PPN_SHIFT

#define MSIREMAP_FLQC           0x010
#define FLQC_IRQEN              (1 << 3)
#define FLQC_CLR                (1 << 2)
#define FLQC_OFLOW_CLR          (1 << 1)
#define FLQC_EN                 0x1

#define MSIREMAP_FLHEAD         0x018
#define FLHEAD_MASK             ((1ULL << 32) - 1)

#define MSIREMAP_FLTAIL         0x020
#define FLTAIL_MASK             ((1ULL << 32) - 1)

#define MSIREMAP_STATUS         0x028
#define STATUS_OFLOW            (1 << 7)
#define STATUS_FAULT            (1 << 6)
#define STATUS_BUSY             (1 << 5)
#define STATUS_QFULL            (1 << 4)
#define STATUS_QEMPTY           (1 << 3)

#define MSIREMAP_CTRL           0x030
#define CTRL_TEST_MODE          (1 << 7)
#define CTRL_PERF_RST           (1 << 6)
#define CTRL_FAULT_CLR          (1 << 5)
#define CTRL_SOFT_RST           (1 << 4)
#define CTRL_FAULT_IRQEN        (1 << 1)
#define CTRL_EN                 0x1

#define MSIREMAP_IMSIC_BASE     0x038
#define IMSIC_BASE_MASK         ((1ULL << 56) - 1)

#define MSIREMAP_IMSIC_STRIDE   0x040
#define IMSIC_STRIDE_MASK       ((1ULL << 12) - 1)

#define MSIREMAP_IMSIC_PRIV_OFF 0x048
#define IMSIC_PRIV_OFF_MASK     ((1ULL << 12) - 1)

#define MSIREMAP_FAULT_INJ      0x050
#define FAULT_INJ_CODE_MASK     ((1ULL << 8) - 1)

#define MSIREMAP_PERF_CTR       0x058
#define PERF_CTR_COUNT_MASK     ((1ULL << 32) - 1)

#define MSIREMAP_PERF_FAULT     0x060
#define PERF_FAULT_COUNT_MASK   ((1ULL << 32) - 1)

#define MSIREMAP_LAST_MSI       0x068
#define LAST_MSI_DATA_MASK      ((1ULL << 32) - 1)

#define MSIREMAP_VERSION        0x070
#define VERSION_MASK            ((1ULL << 8) - 1)

#define MSIREMAP_COALESCE_NS    0x078

#define MSIREMAP_COALESCE_MAX   0x080
#define COALESCE_MAX_MASK       ((1ULL << 16) - 1)

#define MSIREMAP_NOTIF_CTRL     0x088
#define NOTIF_CTRL_PWRDN_EN     0x2
#define NOTIF_CTRL_RESET_EN     0x1

#define MSIREMAP_CHARDEV_CTRL   0x090
#define CHARDEV_CTRL_HEX_DUMP   (1 << 2)
#define CHARDEV_CTRL_VERBOSE    (1 << 1)
#define CHARDEV_CTRL_EN         0x1

#define MSIREMAP_TRACE_MASK     0x098
#define TRACE_MASK_BH           (1 << 7)
#define TRACE_MASK_COALESCE     (1 << 6)
#define TRACE_MASK_FAULT        (1 << 5)
#define TRACE_MASK_DELIVER      (1 << 4)
#define TRACE_MASK_PTE_FETCH    (1 << 2)
#define TRACE_MASK_MSI_RX       (1 << 1)

#define MSIREMAP_BH_PENDING     0x0A0
#define BH_PENDING_COUNT_MASK   ((1ULL << 8) - 1)

#define MSIREMAP_HOTPLUG_SEQ    0x0A8

#define MSIREMAP_DOORBELL       0xF00
#define DOORBELL_MSI_MASK       ((1ULL << 32) - 1)

/* TODO: Add logic to send msi */
static void riscv_msirem_send_msi(RISCVMSIRemState *s)
{
    return;
}

/* Timer functions */
static void reset_timer(RISCVMSIRemState *s)
{
    if (s->coalesce_ns > 0) {
        timer_mod_ns(&s->cb_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->coalesce_ns);
    }
}

static void cb_send_msi(void *opaque)
{
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);
    /* TODO: Add logic to send all msi's curerntly in coalescing buffer */
    for (uint64_t i = 0; i < s->coalesce_max; i++) {
        riscv_msirem_send_msi(s);
    }
    reset_timer(s);
}

/**
 * Fault subsystem
 */

static void fault_sb_to_DRAM(void *opaque)
{
    MemTxResult res = MEMTX_OK;
    RISCVMSIRemState *s = RISCV_MSIREM(opaque);
    hwaddr addr;
    uint64_t qsize, qsize_mask, next_flhead;
    FaultLog *f;

    qsize = (s->flbr >> FLBR_QSIZE_SHIFT) & FLBR_QSIZE_MASK;
    qsize_mask = (1 << qsize) - 1;
    next_flhead = s->flhead + 1;

    /* Checking if there is a difference of 1 entry between next flhead and
     * current fltail. Otherwise DRAM ring buffer full, so not write to DRAM */
    if (((next_flhead + 1) & qsize_mask) != s->fltail) {
        addr = (s->flbr >> FLBR_PPN_SHIFT) & FLBR_PPN_MASK;
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
            /* This should never fail */
            if (res == MEMTX_OK) {
                f = g_queue_pop_head(s->staging_buffer);
                s->flhead = next_flhead & qsize_mask;
                g_free(f);
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

static void fault_logger(RISCVMSIRemState *s, uint8_t fault_code,
                         uint32_t msi_data)
{
    FaultLog *f = g_new(FaultLog, 1);
    f->fault_info = ((uint64_t) msi_data << 32) | fault_code;
    f->timestamp_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    g_queue_push_tail(s->staging_buffer, f);
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
            return msirem->bh_pending;
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
            msirem->ptbr = data;
            break;
        case MSIREMAP_FLBR:
            msirem->flbr = data;
            break;
        case MSIREMAP_FLQC:
            msirem->flqc = data;
            break;
        case MSIREMAP_FLTAIL:
            msirem->fltail = data;
            break;
        case MSIREMAP_CTRL:
            msirem->ctrl = data;
            break;
        case MSIREMAP_IMSIC_BASE:
            msirem->imsic_base = data;
            break;
        case MSIREMAP_IMSIC_STRIDE:
            msirem->imsic_stride = data;
            break;
        case MSIREMAP_IMSIC_PRIV_OFF:
            msirem->imsic_priv_off = data;
            break;
        case MSIREMAP_FAULT_INJ:
            msirem->fault_inj = data;
            break;
        case MSIREMAP_PERF_CTR:
            msirem->perf_ctr = data;
            break;
        case MSIREMAP_PERF_FAULT:
            msirem->perf_fault = data;
            break;
        case MSIREMAP_COALESCE_NS:
            msirem->coalesce_ns = data;
            break;
        case MSIREMAP_COALESCE_MAX:
            msirem->coalesce_max = data;
            break;
        case MSIREMAP_NOTIF_CTRL:
            msirem->notif_ctrl = data;
            break;
        case MSIREMAP_CHARDEV_CTRL:
            msirem->chardev_ctrl = data;
            break;
        case MSIREMAP_TRACE_MASK:
            msirem->trace_mask = data;
            break;
        case MSIREMAP_DOORBELL:
            riscv_msirem_send_msi(msirem);
            msirem->doorbell = data;
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
}

static void riscv_msirem_unrealize(DeviceState *dev)
{
    RISCVMSIRemState *s = RISCV_MSIREM(dev);
    g_queue_free(s->staging_buffer);
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
        VMSTATE_UINT64(bh_pending, RISCVMSIRemState),
        VMSTATE_UINT64(hotplug_seq, RISCVMSIRemState),
        VMSTATE_UINT64(doorbell, RISCVMSIRemState),
        VMSTATE_END_OF_LIST()
    }
};

/* Runstate functions i.e. reset, powerdown, cleanup */
static void common_cleanup(RISCVMSIRemState *s)
{
    /* Flush the coalecing buffer */
    memset(&s->coalescing_buff, 0, sizeof(uint64_t) * COALESCE_BUFF_MAX);
}

static void riscv_msirem_cleanup(Notifier *notifier, void *data)
{
    RISCVMSIRemState *s = container_of(notifier, RISCVMSIRemState,
                                       powerdown);
    /* Perform cleanup */
    common_cleanup(s);
    /* Write power-down marker in fault log pointed by flhead - 1 */
    fault_logger(s, POWER_DOWN_MARKER, NULL);
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
    s->trace_mask = 0;

    common_cleanup(s);
}

static void riscv_msirem_instance_init (Object *obj)
{
    RISCVMSIRemState *s = RISCV_MSIREM(obj);
    timer_init_ns(&s->cb_timer, QEMU_CLOCK_VIRTUAL, cb_send_msi, s);
    s->version = 0x10;
    fault_subsystem_init(s);
    s->powerdown.notify = riscv_msirem_cleanup;
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
    uint8_t value = (uint8_t) (s->ptbr & PTBR_MDOE_MASK);
    const char *val;

    switch (value) {
        case 0:
            val = "OFF";
            break;
        case 1:
            val = "BARE";
            break;
        case 2:
            val = "REMAP-1";
            break;
        case 3:
            val = "REMAP-2";
            break;
        case 4:
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
