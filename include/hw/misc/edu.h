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
} dma_state;

typedef struct {
    bool is_msi;
    MSIMessage msi;
    dma_state dma;
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
    dma_state dma;
    QEMUTimer dma_timer;
    char dma_buf[DMA_SIZE];
    uint64_t dma_mask;
    Chardev *lti_chrdev;
    CharFrontend lti_fe;
    GHashTable *edu_state_history;

#define EDU_PROC_OFFSET         0x100
#define EDU_PROC_VALID          1
#define EDU_PROC_PRIV           2
#define EDU_PROC_EXEC           4
#define EDU_PROC_RSRV_MASK      ((1UL << 9) - 1)
#define EDU_PROC_RSRV_OFFSET    3
#define EDU_PROC_PASID_MASK     ((1UL << 20) - 1)
#define EDU_PROC_PASID_OFFSET   2
    /* | Proc ID | RSRV | E | P | V | */
    /* 31        12     3   2   1   0 */
    uint32_t process_info;
};

void edu_perform_dma(void *opaque, lti_LR_s resp);
