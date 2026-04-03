#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/event_notifier.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/misc/riscv_msirem.h"
#include "migration/vmstate.h"
#include "trace.h"


static uint64_t riscv_msirem_read(void *opaque, hwaddr addr, unsigned size)
{
    if (size != 8) {
        return 0;
    }

    RISCVMSIRemState *msirem = RISCV_MSIREM(opaque);

    switch (addr) {
        case 0x000:
            return msirem->ptbr;
        case 0x008:
            return msirem->flbr;
        case 0x010:
            return msirem->flqc;
        case 0x018:
            return msirem->flhead;
        case 0x020:
            return msirem->fltail;
        case 0x028:
            return msirem->status;
        case 0x030:
            return msirem->ctrl;
        case 0x038:
            return msirem->imsic_base;
        case 0x040:
            return msirem->imsic_stride;
        case 0x048:
            return msirem->imsic_priv_off;
        case 0x058:
            return msirem->perf_ctr;
        case 0x060:
            return msirem->perf_fault;
        case 0x068:
            return msirem->last_msi;
        case 0x070:
            return msirem->version;
        case 0x078:
            return msirem->coalesce_ns;
        case 0x080:
            return msirem->coalesce_max;
        case 0x088:
            return msirem->notif_ctrl;
        case 0x090:
            return msirem->chardev_ctrl;
        case 0x098:
            return msirem->trace_mask;
        case 0x0A0:
            return msirem->bh_pending;
        case 0x0A8:
            return msirem->hotplug_seq;
    }

    /* If addr not not readable, return 0 */
    return 0;
}

static void riscv_msirem_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    if (size != 8) {
        return;
    }

    RISCVMSIRemState *msirem = RISCV_MSIREM(opaque);

    /* If addr not writable, ignore the write */
    switch (addr) {
        case 0x000:
            msirem->ptbr = data;
            break;
        case 0x008:
            msirem->flbr = data;
            break;
        case 0x010:
            msirem->flqc = data;
            break;
        case 0x020:
            msirem->fltail = data;
            break;
        case 0x030:
            msirem->ctrl = data;
            break;
        case 0x038:
            msirem->imsic_base = data;
            break;
        case 0x040:
            msirem->imsic_stride = data;
            break;
        case 0x048:
            msirem->imsic_priv_off = data;
            break;
        case 0x050:
            msirem->fault_inj = data;
            break;
        case 0x058:
            msirem->perf_ctr = data;
            break;
        case 0x060:
            msirem->perf_fault = data;
            break;
        case 0x078:
            msirem->coalesce_ns = data;
            break;
        case 0x080:
            msirem->coalesce_max = data;
            break;
        case 0x088:
            msirem->notif_ctrl = data;
            break;
        case 0x090:
            msirem->chardev_ctrl = data;
            break;
        case 0x098:
            msirem->trace_mask = data;
            break;
        case 0xF00:
            msirem->doorbell = data;
    }
}

static const MemoryRegionOps riscv_msirem_ops = {
    .read = riscv_msirem_read,
    .write = riscv_msirem_write,
    .valid = {
        .min_access_size = 8,
        .max_access_size = 8
    }
};

static void riscv_msirem_realize(DeviceState *dev, Error **errp) {
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
}

static void riscv_msirem_unrealize(DeviceState *dev) {
}

static void riscv_msirem_instance_init (Object *o) {
}

static void riscv_msirem_class_init (ObjectClass *class, void *data) {
    DeviceClass *dc = DEVICE_CLASS(class);

    dc->realize = riscv_msirem_realize;
    dc->unrealize = riscv_msirem_unrealize;
}

static const TypeInfo riscv_msirem_info = {
    .name = TYPE_RISCV_MSIREM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RISCVMSIRemState),
    .instance_init = riscv_msirem_instance_init,
    .class_init = riscv_msirem_class_init
};
