/*
 * QEMU educational PCI device
 *
 * Copyright (c) 2012-2015 Jiri Slaby
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "qemu/timer.h"
#include "qemu/thread.h"
#include "qom/object.h"
#include "qemu/main-loop.h" /* iothread mutex */
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/edu.h"
#include "hw/riscv/riscv_qemu_rtl_intf.h"
#include "trace.h"

static bool edu_msi_enabled(EduState *edu)
{
    return msi_enabled(&edu->pdev);
}

static inline unsigned int msi_nr_vectors(uint16_t flags)
{
    return 1U <<
        ((flags & PCI_MSI_FLAGS_QSIZE) >> ctz32(PCI_MSI_FLAGS_QSIZE));
}

static inline uint8_t msi_flags_off(const PCIDevice* dev)
{
    return dev->msi_cap + PCI_MSI_FLAGS;
}

static inline uint8_t msi_pending_off(const PCIDevice* dev, bool msi64bit)
{
    return dev->msi_cap + (msi64bit ? PCI_MSI_PENDING_64 : PCI_MSI_PENDING_32);
}

static void edu_msi_trans(PCIDevice *dev, unsigned int vector)
{
    EduState *edu = EDU(dev);
    uint16_t flags = pci_get_word(dev->config + msi_flags_off(dev));
    bool msi64bit = flags & PCI_MSI_FLAGS_64BIT;
    unsigned int nr_vectors = msi_nr_vectors(flags);
    MSIMessage msg;
    uint64_t id;

    assert(vector < nr_vectors);
    if (msi_is_masked(dev, vector)) {
        assert(flags & PCI_MSI_FLAGS_MASKBIT);
        pci_long_test_and_set_mask(
            dev->config + msi_pending_off(dev, msi64bit), 1U << vector);
        return;
    }
    msg = msi_get_message(&edu->pdev, 0);

    edu_ghash_entry_s *value = calloc(1, sizeof(edu_ghash_entry_s));
    bool priv = (edu->process_info_msi & EDU_PROC_VALID) ?
                !!(edu->process_info_msi & EDU_PROC_PRIV) : 0;

    memcpy(&value->msi, &msg, sizeof(MSIMessage));
    value->is_msi = true;
    id = rtl_trans_reqt(msg.address, true, priv, 8,
                        !!(edu->process_info_msi & EDU_PROC_VALID),
                        (edu->process_info_msi >> EDU_PROC_PASID_OFFSET) & EDU_PROC_PASID_MASK,
                        &edu->lti_fe);
    g_hash_table_insert(edu->edu_state_history, GINT_TO_POINTER(id), value);
    trace_edu_msi(id, msg.address, msg.data);
}

static void edu_raise_irq(EduState *edu, uint32_t val)
{
    edu->irq_status |= val;
    if (edu->irq_status) {
        if (edu_msi_enabled(edu)) {
            edu_msi_trans(&edu->pdev, 0);
        } else {
            pci_set_irq(&edu->pdev, 1);
        }
    }
}

static void edu_lower_irq(EduState *edu, uint32_t val)
{
    edu->irq_status &= ~val;

    if (!edu->irq_status && !edu_msi_enabled(edu)) {
        pci_set_irq(&edu->pdev, 0);
    }
}

static void edu_check_range(uint64_t xfer_start, uint64_t xfer_size,
                            uint64_t dma_start, uint64_t dma_size)
{
    uint64_t xfer_end = xfer_start + xfer_size;
    uint64_t dma_end = dma_start + dma_size;

    /*
     * 1. ensure we aren't overflowing
     * 2. ensure that xfer is within dma address range
     */
    if (dma_end >= dma_start && xfer_end >= xfer_start &&
        xfer_start >= dma_start && xfer_end <= dma_end) {
        return;
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "EDU: DMA range 0x%016"PRIx64"-0x%016"PRIx64
                  " out of bounds (0x%016"PRIx64"-0x%016"PRIx64")!",
                  xfer_start, xfer_end - 1, dma_start, dma_end - 1);
}

static dma_addr_t edu_clamp_addr(const EduState *edu, dma_addr_t addr)
{
    dma_addr_t res = addr & edu->dma_mask;

    if (addr != res) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "EDU: clamping DMA 0x%016"PRIx64" to 0x%016"PRIx64"!",
                      addr, res);
    }

    return res;
}


void edu_perform_dma(void *opaque, lti_LR_s resp)
{
    EduState *edu = opaque;
    MemTxResult result;
    edu_ghash_entry_s *entry = g_hash_table_lookup(edu->edu_state_history,
                                                   GINT_TO_POINTER(resp.id));
    if (entry == NULL) {
        return;
    }

    trace_edu_perform_dma_ghash_entry(entry->is_msi ? "MSI" : "DMA",
                                      entry->dma.src,
                                      entry->dma.dst,
                                      entry->dma.cnt,
                                      entry->dma.cmd,
                                      entry->msi.address,
                                      entry->msi.data);

    /* Discard LTI request if ABORT received on DMA response */
    if (resp.resp == LTI_RESP_FAULT_ABORT)
    {
        /* Deasserting EDU DMA bit ONLY when DMA transaction returned ABORT */
        if (!entry->is_msi)
            edu->dma.cmd = ~EDU_DMA_RUN;
        goto cleanup;
    }

    if (entry->is_msi) {
        /* MSI translation */
        if (!(resp.mrif_fields & MRIF_VALID)) {
            /* MSI */
            entry->msi.address = resp.spa;
        } else {
            /* MRIF */
            /* MRIF Address = concat(mrif_fields[58:55], spa) */
            uint64_t mrif_addr = (resp.mrif_fields >> MRIF_ADDR_OFFSET) & MRIF_ADDR_MASK;
            mrif_addr = (mrif_addr << 52) | resp.spa;

            /* We generally expect the IO Bridge to updated the MRIF but for
             * now EDU would do it instead */
            uint64_t mrif_eip = address_space_ldq_le(&address_space_memory,
                                                     mrif_addr + 16 * (entry->msi.data / 64),
                                                     MEMTXATTRS_UNSPECIFIED,
                                                     &result);
            /* Failed to read MRIF, discard trasaction */
            if (result != MEMTX_OK) {
                goto cleanup;
            }
            mrif_eip |= (1 << (entry->msi.data % 64));
            address_space_stq_le(&address_space_memory,
                                 mrif_addr + 16 * (entry->msi.data / 64),
                                 mrif_eip, MEMTXATTRS_UNSPECIFIED, &result);
            /* Failed to write in MRIF, discard transaction */
            if (result != MEMTX_OK) {
                goto cleanup;
            }

            /* Setting up NMSI */
            entry->msi.address = (resp.mrif_fields >> MRIF_NPPN_OFFSET) & MRIF_NPPN_MASK;
            entry->msi.address <<= 12;
            entry->msi.data = resp.mrif_fields & MRIF_NID_MASK;
        }

        msi_send_message(PCI_DEVICE(edu), entry->msi);
    } else {
        /* DMA transaction */
        if (!(entry->dma.cmd & EDU_DMA_RUN)) {
            /**
             * If LTI response received but EDU unable to perform DMA,
             * remove request from DMA history
             */
            goto cleanup;
        }

        // EDU_DMA_FROM_PCI = 0, EDU_DMA_TO_PCI = 1
        if (EDU_DMA_DIR(entry->dma.cmd) == EDU_DMA_FROM_PCI) {
            uint64_t dst = entry->dma.dst;
            edu_check_range(dst, entry->dma.cnt, DMA_START, DMA_SIZE);
            dst -= DMA_START;
            pci_dma_read(&edu->pdev, resp.spa, edu->dma_buf + dst, entry->dma.cnt);
        } else {
            uint64_t src = entry->dma.src;
            edu_check_range(src, entry->dma.cnt, DMA_START, DMA_SIZE);
            src -= DMA_START;
            pci_dma_write(&edu->pdev, resp.spa, edu->dma_buf + src, entry->dma.cnt);
        }

        edu->dma.cmd &= ~EDU_DMA_RUN;
        if (entry->dma.cmd & EDU_DMA_IRQ) {
            edu_raise_irq(edu, DMA_IRQ);
        }
    }
cleanup:
    /* Removing entry from hashtable */
    g_hash_table_remove(edu->edu_state_history, GINT_TO_POINTER(resp.id));
}

static void edu_dma_timer(void *opaque)
{
    EduState *edu = opaque;
    uint64_t id;
    // EDU_DMA_FROM_PCI = 0, EDU_DMA_TO_PCI = 1
    bool dma_to_pci = EDU_DMA_DIR(edu->dma.cmd);

    if (!(edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    /* Send DMA request to RTL */
    bool priv = (edu->process_info_dma & EDU_PROC_VALID) ?
                !!(edu->process_info_dma & EDU_PROC_PRIV) : 0;

    edu_ghash_entry_s *value = calloc(1, sizeof(edu_ghash_entry_s));
    memcpy(&value->dma, &edu->dma, sizeof(dma_state));
    value->is_msi = false;
    id = rtl_trans_reqt(edu_clamp_addr(edu, dma_to_pci ? edu->dma.dst : edu->dma.src),
                        EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_TO_PCI,
                        priv, 8, !!(edu->process_info_dma & EDU_PROC_VALID),
                        (edu->process_info_dma >> EDU_PROC_PASID_OFFSET) & EDU_PROC_PASID_MASK,
                        &edu->lti_fe);
    g_hash_table_insert(edu->edu_state_history, GINT_TO_POINTER(id), value);
    trace_edu_dma(id, edu_clamp_addr(edu, dma_to_pci ? edu->dma.dst : edu->dma.src),
                  EDU_DMA_DIR(edu->dma.cmd) == EDU_DMA_TO_PCI ? "WRITE" : "READ");
    trace_edu_ghash_entry(value->is_msi ? "MSI" : "DMA",
                          value->dma.src,
                          value->dma.dst,
                          value->dma.cnt,
                          value->dma.cmd,
                          value->msi.address,
                          value->msi.data);
}

static void dma_rw(EduState *edu, bool write, dma_addr_t *val, dma_addr_t *dma,
                bool timer)
{
    if (write && (edu->dma.cmd & EDU_DMA_RUN)) {
        return;
    }

    if (write) {
        *dma = *val;
    } else {
        *val = *dma;
    }

    if (timer) {
        timer_mod(&edu->dma_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
    }
}

static uint64_t edu_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    EduState *edu = opaque;
    uint64_t val = ~0ULL;

    if (addr < 0x80 && size != 4) {
        return val;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return val;
    }

    switch (addr) {
    case 0x00:
        val = 0x010000edu;
        break;
    case 0x04:
        val = edu->addr4;
        break;
    case 0x08:
        qemu_mutex_lock(&edu->thr_mutex);
        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        val = qatomic_read(&edu->status);
        break;
    case 0x24:
        val = edu->irq_status;
        break;
    case 0x80:
        dma_rw(edu, false, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, false, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, false, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        dma_rw(edu, false, &val, &edu->dma.cmd, false);
        break;
    case EDU_PROC_DMA_OFFSET:
        val = edu->process_info_dma;
        break;
    case EDU_PROC_MSI_OFFSET:
        val = edu->process_info_msi;
        break;
    }

    return val;
}

static void edu_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                unsigned size)
{
    EduState *edu = opaque;

    if (addr < 0x80 && size != 4) {
        return;
    }

    if (addr >= 0x80 && size != 4 && size != 8) {
        return;
    }

    switch (addr) {
    case 0x04:
        edu->addr4 = ~val;
        break;
    case 0x08:
        if (qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) {
            break;
        }
        /* EDU_STATUS_COMPUTING cannot go 0->1 concurrently, because it is only
         * set in this function and it is under the iothread mutex.
         */
        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = val;
        qatomic_or(&edu->status, EDU_STATUS_COMPUTING);
        qemu_cond_signal(&edu->thr_cond);
        qemu_mutex_unlock(&edu->thr_mutex);
        break;
    case 0x20:
        if (val & EDU_STATUS_IRQFACT) {
            qatomic_or(&edu->status, EDU_STATUS_IRQFACT);
            /* Order check of the COMPUTING flag after setting IRQFACT.  */
            smp_mb__after_rmw();
        } else {
            qatomic_and(&edu->status, ~EDU_STATUS_IRQFACT);
        }
        break;
    case 0x60:
        edu_raise_irq(edu, val);
        break;
    case 0x64:
        edu_lower_irq(edu, val);
        break;
    case 0x80:
        dma_rw(edu, true, &val, &edu->dma.src, false);
        break;
    case 0x88:
        dma_rw(edu, true, &val, &edu->dma.dst, false);
        break;
    case 0x90:
        dma_rw(edu, true, &val, &edu->dma.cnt, false);
        break;
    case 0x98:
        if (!(val & EDU_DMA_RUN)) {
            break;
        }
        dma_rw(edu, true, &val, &edu->dma.cmd, true);
        break;
    case EDU_PROC_DMA_OFFSET:
        edu->process_info_dma = val & ~(EDU_PROC_RSRV_MASK << EDU_PROC_RSRV_OFFSET);
        break;
    case EDU_PROC_MSI_OFFSET:
        edu->process_info_msi = val & ~(EDU_PROC_RSRV_MASK << EDU_PROC_RSRV_OFFSET);
        break;
    }
}

static const MemoryRegionOps edu_mmio_ops = {
    .read = edu_mmio_read,
    .write = edu_mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },

};

/*
 * We purposely use a thread, so that users are forced to wait for the status
 * register.
 */
static void *edu_fact_thread(void *opaque)
{
    EduState *edu = opaque;

    while (1) {
        uint32_t val, ret = 1;

        qemu_mutex_lock(&edu->thr_mutex);
        while ((qatomic_read(&edu->status) & EDU_STATUS_COMPUTING) == 0 &&
                        !edu->stopping) {
            qemu_cond_wait(&edu->thr_cond, &edu->thr_mutex);
        }

        if (edu->stopping) {
            qemu_mutex_unlock(&edu->thr_mutex);
            break;
        }

        val = edu->fact;
        qemu_mutex_unlock(&edu->thr_mutex);

        while (val > 0) {
            ret *= val--;
        }

        /*
         * We should sleep for a random period here, so that students are
         * forced to check the status properly.
         */

        qemu_mutex_lock(&edu->thr_mutex);
        edu->fact = ret;
        qemu_mutex_unlock(&edu->thr_mutex);
        qatomic_and(&edu->status, ~EDU_STATUS_COMPUTING);

        /* Clear COMPUTING flag before checking IRQFACT.  */
        smp_mb__after_rmw();

        if (qatomic_read(&edu->status) & EDU_STATUS_IRQFACT) {
            bql_lock();
            edu_raise_irq(edu, FACT_IRQ);
            bql_unlock();
        }
    }

    return NULL;
}

static void pci_edu_realize(PCIDevice *pdev, Error **errp)
{
    EduState *edu = EDU(pdev);
    uint8_t *pci_conf = pdev->config;

    pci_config_set_interrupt_pin(pci_conf, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    timer_init_ms(&edu->dma_timer, QEMU_CLOCK_VIRTUAL, edu_dma_timer, edu);

    qemu_mutex_init(&edu->thr_mutex);
    qemu_cond_init(&edu->thr_cond);
    qemu_thread_create(&edu->thread, "edu", edu_fact_thread,
                       edu, QEMU_THREAD_JOINABLE);

    /* Initializing lti frontend */
    qemu_chr_fe_init(&edu->lti_fe, edu->lti_chrdev, errp);
    qemu_chr_fe_set_handlers(&edu->lti_fe, can_read_rtl_trans_resp,
                             read_rtl_trans_resp, lti_event_handler,
                             NULL, edu, NULL, true);
    memory_region_init_io(&edu->mmio, OBJECT(edu), &edu_mmio_ops, edu,
                          "edu-mmio", 1 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &edu->mmio);
    edu->edu_state_history = g_hash_table_new_full(NULL, NULL, NULL, free);
}

static void pci_edu_uninit(PCIDevice *pdev)
{
    EduState *edu = EDU(pdev);

    qemu_mutex_lock(&edu->thr_mutex);
    edu->stopping = true;
    qemu_mutex_unlock(&edu->thr_mutex);
    qemu_cond_signal(&edu->thr_cond);
    qemu_thread_join(&edu->thread);

    qemu_cond_destroy(&edu->thr_cond);
    qemu_mutex_destroy(&edu->thr_mutex);

    timer_del(&edu->dma_timer);
    msi_uninit(pdev);
}

static void edu_instance_finalize(Object *obj)
{
    EduState *edu = EDU(obj);
    g_hash_table_destroy(edu->edu_state_history);
}

static void edu_instance_init(Object *obj)
{
    EduState *edu = EDU(obj);

    edu->dma_mask = (1UL << 28) - 1;
    object_property_add_uint64_ptr(obj, "dma_mask",
                                   &edu->dma_mask, OBJ_PROP_FLAG_READWRITE);
    // TODO: TYPE_CHARDEV -> TYPE_CHARDEV_MUX when using multiple devices
    object_property_add_link(obj, "lti_intf", TYPE_CHARDEV,
                             (Object **)&edu->lti_chrdev,
                             qdev_prop_allow_set_link_before_realize,
                             0);
}

static void edu_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize = pci_edu_realize;
    k->exit = pci_edu_uninit;
    k->vendor_id = PCI_VENDOR_ID_QEMU;
    k->device_id = 0x11e8;
    k->revision = 0x10;
    k->class_id = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo edu_types[] = {
    {
        .name          = TYPE_PCI_EDU_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(EduState),
        .instance_init = edu_instance_init,
        .instance_finalize = edu_instance_finalize,
        .class_init    = edu_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    }
};

DEFINE_TYPES(edu_types);
