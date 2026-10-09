#include "qom/object.h"
#include "hw/pci/pci_device.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "hw/pci/msi.h"
#include "hw/riscv/riscv_qemu_rtl_intf.h"

#define TYPE_PCI_EDU_DEVICE "edu"
typedef struct EduState EduState;
DECLARE_INSTANCE_CHECKER(EduState, EDU,
                         TYPE_PCI_EDU_DEVICE)

#define FACT_IRQ        0x00000001
#define DMA_IRQ         0x00000100

#define DMA_START       0x40000
#define DMA_SIZE        4096

typedef struct {
    dma_addr_t src;
    dma_addr_t dst;
    dma_addr_t cnt;
    dma_addr_t cmd;
} dma_state_s;

typedef union {
        struct {
            uint32_t priv : 1;
            uint32_t exec : 1;
            uint32_t process_id : 20;
        } fields;
        uint32_t raw;
} process_state_s;

typedef struct {
    dma_state_s dma;

    // MSI fields
    bool is_msi;
    MSIMessage msi;

    // Process Info fields
    bool pidv;
    process_state_s proc_info;
} edu_ghash_entry_s;

struct EduState {
    PCIDevice pdev;
    MemoryRegion mmio;

    QemuThread thread;
    QemuMutex thr_mutex;
    QemuCond thr_cond;
    bool stopping;

    uint32_t addr4;
    uint32_t fact;
#define EDU_STATUS_COMPUTING    0x01
#define EDU_STATUS_IRQFACT      0x80
    uint32_t status;

    uint32_t irq_status;

#define EDU_DMA_RUN             0x1
#define EDU_DMA_DIR(cmd)        (((cmd) & 0x2) >> 1)
# define EDU_DMA_FROM_PCI       0
# define EDU_DMA_TO_PCI         1
#define EDU_DMA_IRQ             0x4
    QemuThread dma_thread;
    dma_state_s dma;
    QEMUTimer dma_timer;
    char dma_buf[DMA_SIZE];
    uint64_t dma_mask;
    GHashTable *edu_state_history;

#define EDU_PROC_VALID          (1UL << 20)
#define EDU_PROC_EXEC           (1UL << 21)
#define EDU_PROC_PRIV           (1UL << 22)
#define EDU_PROC_PASID_BITS     20
#define EDU_PROC_PASID_MASK     ((1UL << EDU_PROC_PASID_BITS) - 1)
#define EDU_PROC_RSVD_OFFSET    23
#define EDU_PROC_RSVD_MASK      ((1UL << 9) - 1)
    /* | RSVD | P | E | V | Process ID | */
    /* 31    23  22  21  20  19         0 */

    uint32_t process_info_dma;
    uint32_t process_info_msi;
    // Whether device has been registered by RTL IOMMU
    bool registered;;
};

void edu_perform_dma(void *opaque, lti_LR_s resp);
