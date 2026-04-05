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
#define MSIREM_SIZE         (1 << 12)
#define COALESCE_BUFF_MAX   256
#define BH_PENDING_MAX      64

/* MSI Remapper Alias region size */
#define MSIREM_ALIAS_SIZE   256
#define MSIREM_ALIAS_OFFSET 0x800

/**
 * MSI Remapper
 */
struct RISCVMSIRemState {
    /*< private >*/
    struct rcu_head rcu;
    SysBusDevice parent;

    qemu_irq fault_irq;

    GQueue *staging_buffer;
    QEMUBH *staging_buffer_bh;  /*< Stagging Buffer bottom half */

    /* Coalescing buffer */
    uint64_t cb[COALESCE_BUFF_MAX];
    uint64_t cb_count;
    QEMUTimer cb_timer;         /*< Coalescing Buffer timer */

    QemuMutex mutex;        /*< Page Table Mutex for RCU */
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
    uint64_t hotplug_seq;
    uint64_t doorbell;
};

enum FaultCodes {
    FAULT_INVALID_PTE       = 0x01,
    FAULT_UNEXPECTED_LEAF   = 0x02,
    FAULT_EXPECTED_LEAF     = 0x03,
    FAULT_RESERVED_BITS     = 0x04,
    FAULT_INVALID_PRIV      = 0x05,
    FAULT_ACCESS_ERROR      = 0x06,
    FAULT_MODE_OFF          = 0x07,
    FAULT_DEVICE_DISABLE    = 0x08,
    POWER_DOWN_MARKER       = 0xFE,
    FAULT_INTERNAL_ERROR    = 0xFF
};

enum TranslationMode {
    OFF,
    BARE,
    REMAP_1,
    REMAP_2,
    REMAP_3,
};

enum PrivLevel {
    MMODE,
    SMODE,
    VSMODE,
};

typedef struct FaultLog {
    uint64_t fault_info;
    uint64_t timestamp_ns;
} FaultLog;

DeviceState *riscv_msirem_create(hwaddr addr);
