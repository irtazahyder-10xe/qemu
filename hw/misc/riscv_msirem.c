#include "qemu/osdep.h"
#include "trace.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "qemu/module.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "qemu/event_notifier.h"
#include "hw/misc/riscv_msirem.h"
#include "qemu/main-loop.h"

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
            /* FIX: Remove this later */
            fault_logger(msirem, 0x00, 0x100);
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
}

static void riscv_msirem_instance_init (Object *obj)
{
    RISCVMSIRemState *s = RISCV_MSIREM(obj);
    s->version = 0x10;
}

static void riscv_msirem_class_init (ObjectClass *class, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);

    dc->realize = riscv_msirem_realize;
    dc->desc = "MSI Remapper";
    dc->unrealize = riscv_msirem_unrealize;
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
