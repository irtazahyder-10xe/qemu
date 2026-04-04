/*
 * RISC-V MSI Remapper interface
 */

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "chardev/char-fe.h"


#define TYPE_RISCV_MSIREM "riscv.msiremap"

typedef struct RISCVMSIRemState RISCVMSIRemState;
DECLARE_INSTANCE_CHECKER(RISCVMSIRemState, RISCV_MSIREM, TYPE_RISCV_MSIREM)

/* MSI Remapper Total size */
#define MSIREM_SIZE (1 << 12)

/* MSI Remapper Alias region size */
#define MSIREM_ALIAS_SIZE   256
#define MSIREM_ALIAS_OFFSET 0x800

/**
 * MSI Remapper
 */
struct RISCVMSIRemState {
    /*< private >*/
    SysBusDevice parent;

    qemu_irq fault_irq;

    GQueue *staging_buffer;
    QEMUBH *staging_buffer_bh;  /*< Stagging Buffer bottom half */

    QEMUTimer cb_timer;         /*< Coalescing Buffer timer */

    QemuMutex ptb_mutex;        /*< Page Table Mutex for RCU */
    Notifier powerdown;

    CharBackend debug_logger;

    uint64_t total_pgtb_walk;

    /*< public >*/
    MemoryRegion mmio;
    MemoryRegion regfile;
    MemoryRegion alias;

    /* MMIO Registers */
    uint64_t ptbr;
    uint64_t flbr;
    uint64_t flqc;
    uint64_t flhead;
    uint64_t fltail;
    uint64_t status;
    uint64_t ctrl;
    uint64_t imsic_base;
    uint64_t imsic_stride;
    uint64_t imsic_priv_off;
    uint64_t fault_inj;
    uint64_t perf_ctr;
    uint64_t perf_fault;
    uint64_t last_msi;
    uint64_t version;
    uint64_t coalesce_ns;
    uint64_t coalesce_max;
    uint64_t notif_ctrl;
    uint64_t chardev_ctrl;
    uint64_t trace_mask;
    uint64_t bh_pending;
    uint64_t hotplug_seq;
    uint64_t doorbell;
};

/**
 * Page Table Entry
 */
typedef struct PTE {
    bool valid;
    bool leaf;
    uint64_t ppn;
    uint64_t eiid;
    uint8_t hart_id;
    uint8_t guest_idx;
    uint8_t priv;
} PTE;

typedef struct FaultLog {
    uint64_t fault_info;
    uint64_t timestamp_ns;
} FaultLog;

DeviceState *riscv_msirem_create(hwaddr addr);
