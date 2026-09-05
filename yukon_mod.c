#include <stddef.h>
#include <stdint.h>
#include "yukon.h"
#include "pci_enum.h"
#include "pci.h"
#include "net.h"

/* Manifest read by pci_peek_module_manifest(). */
const uint16_t cact_pci_vendor_id = MARVELL_VENDOR_ID;
const uint16_t cact_pci_device_ids[] = {MARVELL_YUKON_88E8040_DID, 0};

extern void printk(const char *fmt, ...);
extern void printk_hex(uint32_t v);

extern uint32_t pci_read_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
extern void pci_write_config_dword(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t val);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);
extern void register_netdev(net_driver_t *drv);
extern void unregister_netdev(net_driver_t *drv);
extern net_driver_t *active_nic;
extern void vmm_map(uint32_t *pd, uint32_t virt, uint32_t phys, int flags);
extern skb_t *skb_alloc(void);
extern void kfree_skb(skb_t *skb);
extern uint8_t *skb_put(skb_t *skb, uint16_t len);
extern uint8_t *skb_data(skb_t *skb);
extern uint16_t skb_len(skb_t *skb);
extern void netif_rx(skb_t *skb);
extern void *kmalloc_aligned(uint32_t size, uint32_t align);
extern void kfree(void *p);


typedef struct {
    uint8_t bus;
    uint8_t dev;
    uint8_t fn;
    uint8_t irq_line;
    uint8_t attached;
    uint32_t saved_pci_cmd;
    uint32_t mmio_base;
    uint32_t mmio_size;
    uint8_t mac[6];
    uint8_t native_mac_ok;
    uint8_t native_tx_ok;
    uint8_t native_rx_ok;
    uint16_t tx_put;
    uint16_t tx_cons;
    uint16_t rx_put;
    uint16_t rx_cons;
    uint16_t st_idx;
} yukon_state_t;

static yukon_state_t g_yukon;
static net_driver_t *g_prev_nic;

#define YUKON_SW_LOOP_Q 64u
typedef struct {
    uint16_t len;
    uint8_t buf[1600];
} yukon_sw_frame_t;
static yukon_sw_frame_t g_sw_q[YUKON_SW_LOOP_Q];
static uint16_t g_sw_q_head;
static uint16_t g_sw_q_tail;

typedef struct __attribute__((packed)) {
    uint32_t addr;
    uint16_t length;
    uint8_t ctrl;
    uint8_t opcode;
} yukon_tx_le_t;

#define YUKON_TX_RING_SIZE 64u
#define YUKON_TX_BUF_SIZE  1600u
static yukon_tx_le_t *g_tx_le;
static uint8_t *g_tx_bufs[YUKON_TX_RING_SIZE];
static yukon_tx_le_t *g_rx_le;
static uint8_t *g_rx_bufs[YUKON_TX_RING_SIZE];

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint16_t length;
    uint8_t css;
    uint8_t opcode;
} yukon_status_le_t;

#define YUKON_ST_RING_SIZE 256u
static yukon_status_le_t *g_st_le;

#define B2_MAC_1 0x0100u
#define B2_MAC_2 0x0108u
#define YUKON_MMIO_WINDOW 0x4000u
#define Y2_B8_PREF_REGS 0x0450u
#define Y2_QADDR(q, reg) (Y2_B8_PREF_REGS + (q) + (reg))
#define PREF_UNIT_CTRL 0x00u
#define PREF_UNIT_LAST_IDX 0x04u
#define PREF_UNIT_ADDR_LO 0x08u
#define PREF_UNIT_ADDR_HI 0x0cu
#define PREF_UNIT_GET_IDX 0x10u
#define PREF_UNIT_PUT_IDX 0x14u
#define PREF_UNIT_OP_ON  (1u << 3)
#define PREF_UNIT_RST_CLR (1u << 1)
#define PREF_UNIT_RST_SET (1u << 0)

#define B8_Q_REGS 0x0400u
#define Q_ADDR(q, off) (B8_Q_REGS + (q) + (off))
#define Q_CSR 0x34u
#define Q_R1 0x0000u
#define Q_XA1 0x0280u

#define B16_RAM_REGS 0x0800u
#define RB_ADDR(off, q) ((B16_RAM_REGS) + (q) + (off))
#define RB_CTRL 0x28u
#define RB_ENA_OP_MD (1u << 3)
#define RB_RST_CLR   (1u << 1)
#define RB_RST_SET   (1u << 0)

#define BMU_CLR_IRQ_PAR (1u << 11)
#define BMU_CLR_IRQ_CHK (1u << 10)
#define BMU_START       (1u << 8)
#define BMU_FIFO_ENA    (1u << 5)
#define BMU_FIFO_RST    (1u << 4)
#define BMU_OP_ON       (1u << 3)
#define BMU_OP_OFF      (1u << 2)
#define BMU_RST_CLR     (1u << 1)
#define BMU_RST_SET     (1u << 0)
#define BMU_CLR_RESET (BMU_FIFO_RST | BMU_OP_OFF | BMU_RST_CLR)
#define BMU_OPER_INIT (BMU_CLR_IRQ_PAR | BMU_CLR_IRQ_CHK | BMU_START | BMU_FIFO_ENA | BMU_OP_ON)

#define HW_OWNER (1u << 7)
#define OP_PACKET 0x41u
#define OP_RXSTAT 0x60u

#define STAT_CTRL 0x0e80u
#define STAT_LIST_ADDR_LO 0x0e88u
#define STAT_LIST_ADDR_HI 0x0e8cu
#define STAT_PUT_IDX 0x0e9cu
#define SC_STAT_CLR_IRQ (1u << 4)
#define SC_STAT_OP_ON   (1u << 3)
#define SC_STAT_RST_CLR (1u << 1)
#define SC_STAT_RST_SET (1u << 0)

#define PAGE_PRESENT 0x1
#define PAGE_RW      0x2
#define PAGE_PCD     0x10
#define PAGE_PWT     0x8

static inline uint32_t *get_current_pd(void) {
    uint32_t val;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(val));
    return (uint32_t *)val;
}

static inline uint32_t yukon_mmio_read32(uint32_t off) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(g_yukon.mmio_base + off);
    return *p;
}

static inline uint16_t yukon_mmio_read16(uint32_t off) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)(g_yukon.mmio_base + off);
    return *p;
}

static inline void yukon_mmio_write32(uint32_t off, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(g_yukon.mmio_base + off);
    *p = val;
}

static inline void yukon_mmio_write16(uint32_t off, uint16_t val) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)(g_yukon.mmio_base + off);
    *p = val;
}

static inline void yukon_mmio_write8(uint32_t off, uint8_t val) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(g_yukon.mmio_base + off);
    *p = val;
}

static void yukon_prefetch_init(uint32_t qaddr, uintptr_t ring_addr, uint16_t last_idx) {
    yukon_mmio_write32(Y2_QADDR(qaddr, PREF_UNIT_CTRL), PREF_UNIT_RST_SET);
    yukon_mmio_write32(Y2_QADDR(qaddr, PREF_UNIT_CTRL), PREF_UNIT_RST_CLR);
    yukon_mmio_write32(Y2_QADDR(qaddr, PREF_UNIT_ADDR_HI), 0);
    yukon_mmio_write32(Y2_QADDR(qaddr, PREF_UNIT_ADDR_LO), (uint32_t)ring_addr);
    yukon_mmio_write16(Y2_QADDR(qaddr, PREF_UNIT_LAST_IDX), last_idx);
    yukon_mmio_write32(Y2_QADDR(qaddr, PREF_UNIT_CTRL), PREF_UNIT_OP_ON);
}

static int yukon_setup_native_tx(void) {
    uint32_t i;
    if (g_yukon.mmio_base == 0)
        return -1;

    g_tx_le = (yukon_tx_le_t *)kmalloc_aligned(sizeof(yukon_tx_le_t) * YUKON_TX_RING_SIZE, 4096);
    if (!g_tx_le)
        return -1;
    memset(g_tx_le, 0, sizeof(yukon_tx_le_t) * YUKON_TX_RING_SIZE);

    for (i = 0; i < YUKON_TX_RING_SIZE; i++) {
        g_tx_bufs[i] = (uint8_t *)kmalloc_aligned(YUKON_TX_BUF_SIZE, 16);
        if (!g_tx_bufs[i])
            return -1;
    }

    g_yukon.tx_put = 0;
    g_yukon.tx_cons = 0;

    yukon_prefetch_init(Q_XA1, (uintptr_t)g_tx_le, (uint16_t)(YUKON_TX_RING_SIZE - 1u));
    yukon_mmio_write8(RB_ADDR(RB_CTRL, Q_XA1), (uint8_t)(RB_RST_SET));
    yukon_mmio_write8(RB_ADDR(RB_CTRL, Q_XA1), (uint8_t)(RB_RST_CLR | RB_ENA_OP_MD));
    yukon_mmio_write32(Q_ADDR(Q_XA1, Q_CSR), BMU_CLR_RESET);
    yukon_mmio_write32(Q_ADDR(Q_XA1, Q_CSR), BMU_OPER_INIT);
    g_yukon.native_tx_ok = 1;
    return 0;
}

static int yukon_setup_native_status(void) {
    if (g_yukon.mmio_base == 0)
        return -1;
    g_st_le = (yukon_status_le_t *)kmalloc_aligned(sizeof(yukon_status_le_t) * YUKON_ST_RING_SIZE, 4096);
    if (!g_st_le)
        return -1;
    memset(g_st_le, 0, sizeof(yukon_status_le_t) * YUKON_ST_RING_SIZE);
    g_yukon.st_idx = 0;
    yukon_mmio_write32(STAT_CTRL, SC_STAT_RST_SET);
    yukon_mmio_write32(STAT_CTRL, SC_STAT_RST_CLR);
    yukon_mmio_write32(STAT_LIST_ADDR_HI, 0);
    yukon_mmio_write32(STAT_LIST_ADDR_LO, (uint32_t)(uintptr_t)g_st_le);
    yukon_mmio_write32(STAT_CTRL, SC_STAT_OP_ON);
    return 0;
}

static int yukon_setup_native_rx(void) {
    uint32_t i;
    if (g_yukon.mmio_base == 0)
        return -1;

    g_rx_le = (yukon_tx_le_t *)kmalloc_aligned(sizeof(yukon_tx_le_t) * YUKON_TX_RING_SIZE, 4096);
    if (!g_rx_le)
        return -1;
    memset(g_rx_le, 0, sizeof(yukon_tx_le_t) * YUKON_TX_RING_SIZE);

    for (i = 0; i < YUKON_TX_RING_SIZE; i++) {
        g_rx_bufs[i] = (uint8_t *)kmalloc_aligned(YUKON_TX_BUF_SIZE, 16);
        if (!g_rx_bufs[i])
            return -1;
    }

    g_yukon.rx_put = 0;
    g_yukon.rx_cons = 0;
    for (i = 0; i < YUKON_TX_RING_SIZE; i++) {
        g_rx_le[i].addr = (uint32_t)(uintptr_t)g_rx_bufs[i];
        g_rx_le[i].length = YUKON_TX_BUF_SIZE;
        g_rx_le[i].ctrl = 0;
        g_rx_le[i].opcode = (uint8_t)(OP_PACKET | HW_OWNER);
        g_yukon.rx_put = (uint16_t)((g_yukon.rx_put + 1u) % YUKON_TX_RING_SIZE);
    }

    yukon_prefetch_init(Q_R1, (uintptr_t)g_rx_le, (uint16_t)(YUKON_TX_RING_SIZE - 1u));
    yukon_mmio_write8(RB_ADDR(RB_CTRL, Q_R1), (uint8_t)(RB_RST_SET));
    yukon_mmio_write8(RB_ADDR(RB_CTRL, Q_R1), (uint8_t)(RB_RST_CLR | RB_ENA_OP_MD));
    yukon_mmio_write32(Q_ADDR(Q_R1, Q_CSR), BMU_CLR_RESET);
    yukon_mmio_write32(Q_ADDR(Q_R1, Q_CSR), BMU_OPER_INIT);
    yukon_mmio_write16(Y2_QADDR(Q_R1, PREF_UNIT_PUT_IDX), g_yukon.rx_put);
    g_yukon.native_rx_ok = 1;
    return 0;
}

static int yukon_map_mmio(uint32_t mmio_base) {
    uint32_t start = mmio_base & ~0xFFFu;
    uint32_t end = (mmio_base + YUKON_MMIO_WINDOW + 0xFFFu) & ~0xFFFu;
    uint32_t va;
    for (va = start; va < end; va += 0x1000u)
        vmm_map(get_current_pd(), va, va, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
    g_yukon.mmio_base = mmio_base;
    g_yukon.mmio_size = YUKON_MMIO_WINDOW;
    return 0;
}

static int yukon_read_hw_mac(uint8_t out[6]) {
    uint32_t lo;
    uint32_t hi;
    int i;
    int all_zero = 1;
    int all_ff = 1;

    if (!out || g_yukon.mmio_base == 0)
        return -1;

    lo = yukon_mmio_read32(B2_MAC_1);
    hi = yukon_mmio_read32(B2_MAC_2);
    out[0] = (uint8_t)(lo & 0xFFu);
    out[1] = (uint8_t)((lo >> 8) & 0xFFu);
    out[2] = (uint8_t)((lo >> 16) & 0xFFu);
    out[3] = (uint8_t)((lo >> 24) & 0xFFu);
    out[4] = (uint8_t)(hi & 0xFFu);
    out[5] = (uint8_t)((hi >> 8) & 0xFFu);

    for (i = 0; i < 6; i++) {
        if (out[i] != 0x00u) all_zero = 0;
        if (out[i] != 0xFFu) all_ff = 0;
    }
    if (all_zero || all_ff)
        return -1;
    return 0;
}

static int yukon_send_stub(skb_t *skb) {
    uint16_t len;
    uint16_t next;
    if (g_yukon.native_tx_ok && g_tx_le && skb) {
        len = skb_len(skb);
        if (len > 0 && len <= YUKON_TX_BUF_SIZE) {
            next = (uint16_t)((g_yukon.tx_put + 1u) % YUKON_TX_RING_SIZE);
            if (next != g_yukon.tx_cons) {
                memcpy(g_tx_bufs[g_yukon.tx_put], skb_data(skb), len);
                g_tx_le[g_yukon.tx_put].addr = (uint32_t)(uintptr_t)g_tx_bufs[g_yukon.tx_put];
                g_tx_le[g_yukon.tx_put].length = len;
                g_tx_le[g_yukon.tx_put].ctrl = 0;
                g_tx_le[g_yukon.tx_put].opcode = (uint8_t)(OP_PACKET | HW_OWNER);
                g_yukon.tx_put = next;
                yukon_mmio_write16(Y2_QADDR(Q_XA1, PREF_UNIT_PUT_IDX), g_yukon.tx_put);
                kfree_skb(skb);
                return 0;
            }
        }
    }

    if (g_prev_nic && g_prev_nic->send)
        return g_prev_nic->send(skb);

    if (!skb)
        return -1;

    len = skb_len(skb);
    if (len == 0 || len > (uint16_t)sizeof(g_sw_q[0].buf)) {
        kfree_skb(skb);
        return -1;
    }

    next = (uint16_t)((g_sw_q_head + 1u) % YUKON_SW_LOOP_Q);
    if (next == g_sw_q_tail) {
        kfree_skb(skb);
        return -1;
    }

    g_sw_q[g_sw_q_head].len = len;
    memcpy(g_sw_q[g_sw_q_head].buf, skb_data(skb), len);
    g_sw_q_head = next;
    kfree_skb(skb);
    return 0;
}

static void yukon_poll_stub(void) {
    if (g_yukon.native_tx_ok && g_yukon.mmio_base) {
        uint16_t hw_get = yukon_mmio_read16(Y2_QADDR(Q_XA1, PREF_UNIT_GET_IDX));
        g_yukon.tx_cons = (uint16_t)(hw_get % YUKON_TX_RING_SIZE);
    }
    if (g_yukon.native_rx_ok && g_st_le && g_yukon.mmio_base) {
        uint16_t put = (uint16_t)(yukon_mmio_read16(STAT_PUT_IDX) % YUKON_ST_RING_SIZE);
        while (g_yukon.st_idx != put) {
            yukon_status_le_t *le = &g_st_le[g_yukon.st_idx];
            uint8_t opcode = le->opcode;
            uint16_t length = le->length;
            if (!(opcode & HW_OWNER))
                break;
            opcode &= (uint8_t)~HW_OWNER;
            if (opcode == OP_RXSTAT && length > 0 && length <= YUKON_TX_BUF_SIZE) {
                skb_t *skb = skb_alloc();
                if (skb) {
                    uint8_t *dst = skb_put(skb, length);
                    if (dst) {
                        memcpy(dst, g_rx_bufs[g_yukon.rx_cons], length);
                        netif_rx(skb);
                    } else {
                        kfree_skb(skb);
                    }
                }
                g_rx_le[g_yukon.rx_cons].addr = (uint32_t)(uintptr_t)g_rx_bufs[g_yukon.rx_cons];
                g_rx_le[g_yukon.rx_cons].length = YUKON_TX_BUF_SIZE;
                g_rx_le[g_yukon.rx_cons].ctrl = 0;
                g_rx_le[g_yukon.rx_cons].opcode = (uint8_t)(OP_PACKET | HW_OWNER);
                g_yukon.rx_cons = (uint16_t)((g_yukon.rx_cons + 1u) % YUKON_TX_RING_SIZE);
                g_yukon.rx_put = (uint16_t)((g_yukon.rx_put + 1u) % YUKON_TX_RING_SIZE);
                yukon_mmio_write16(Y2_QADDR(Q_R1, PREF_UNIT_PUT_IDX), g_yukon.rx_put);
            }
            le->opcode = 0;
            g_yukon.st_idx = (uint16_t)((g_yukon.st_idx + 1u) % YUKON_ST_RING_SIZE);
        }
        yukon_mmio_write32(STAT_CTRL, SC_STAT_CLR_IRQ);
    }

    while (g_sw_q_tail != g_sw_q_head) {
        skb_t *skb = skb_alloc();
        uint8_t *dst;
        uint16_t len = g_sw_q[g_sw_q_tail].len;
        if (!skb)
            break;
        dst = skb_put(skb, len);
        if (!dst) {
            kfree_skb(skb);
            break;
        }
        memcpy(dst, g_sw_q[g_sw_q_tail].buf, len);
        netif_rx(skb);
        g_sw_q_tail = (uint16_t)((g_sw_q_tail + 1u) % YUKON_SW_LOOP_Q);
    }

    if (g_prev_nic && g_prev_nic->poll)
        g_prev_nic->poll();
}

static void yukon_get_mac_stub(mac_addr_t *out) {
    if (!out)
        return;
    if (g_yukon.native_mac_ok) {
        out->b[0] = g_yukon.mac[0];
        out->b[1] = g_yukon.mac[1];
        out->b[2] = g_yukon.mac[2];
        out->b[3] = g_yukon.mac[3];
        out->b[4] = g_yukon.mac[4];
        out->b[5] = g_yukon.mac[5];
        return;
    }
    if (g_prev_nic && g_prev_nic->get_mac) {
        g_prev_nic->get_mac(out);
        return;
    }
    memset(out, 0, sizeof(*out));
}

static net_driver_t g_yukon_driver = {
    .name = "Marvell Yukon-2 (88E8040) [stub]",
    .send = yukon_send_stub,
    .poll = yukon_poll_stub,
    .get_mac = yukon_get_mac_stub,
};

static void yukon_detach(void) {
    if (!g_yukon.attached)
        return;

    unregister_netdev(&g_yukon_driver);
    pci_write_config_dword(g_yukon.bus, g_yukon.dev, g_yukon.fn, 0x04, g_yukon.saved_pci_cmd);
    g_yukon.attached = 0;
    g_prev_nic = NULL;
    g_yukon.mmio_base = 0;
    g_yukon.mmio_size = 0;
    g_yukon.native_mac_ok = 0;
    g_yukon.native_tx_ok = 0;
    g_yukon.native_rx_ok = 0;
    if (g_tx_le) {
        kfree(g_tx_le);
        g_tx_le = NULL;
    }
    {
        uint32_t i;
        for (i = 0; i < YUKON_TX_RING_SIZE; i++) {
            if (g_tx_bufs[i]) {
                kfree(g_tx_bufs[i]);
                g_tx_bufs[i] = NULL;
            }
        }
    }
    if (g_rx_le) {
        kfree(g_rx_le);
        g_rx_le = NULL;
    }
    {
        uint32_t i;
        for (i = 0; i < YUKON_TX_RING_SIZE; i++) {
            if (g_rx_bufs[i]) {
                kfree(g_rx_bufs[i]);
                g_rx_bufs[i] = NULL;
            }
        }
    }
    if (g_st_le) {
        kfree(g_st_le);
        g_st_le = NULL;
    }
    g_sw_q_head = 0;
    g_sw_q_tail = 0;
    printk("6" "yukon (kmod): detached");
}

int pci_driver_probe(pci_device_t *pdev) {
    uint32_t cmd32;
    uint16_t cmd16;

    if (!pdev)
        return -1;

    yukon_detach();
    memset(&g_yukon, 0, sizeof(g_yukon));

    g_yukon.bus = pdev->bus;
    g_yukon.dev = pdev->dev;
    g_yukon.fn = pdev->fn;
    g_yukon.irq_line = pdev->irq_line;
    g_prev_nic = active_nic;
    g_yukon.mmio_base = 0;
    g_yukon.mmio_size = 0;
    g_yukon.native_mac_ok = 0;
    g_yukon.native_tx_ok = 0;
    g_yukon.native_rx_ok = 0;

    if (!pdev->bars[0].is_io && pdev->bars[0].base)
        yukon_map_mmio(pdev->bars[0].base);
    else if (!pdev->bars[1].is_io && pdev->bars[1].base)
        yukon_map_mmio(pdev->bars[1].base);

    cmd32 = pci_read_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04);
    g_yukon.saved_pci_cmd = cmd32;
    cmd16 = (uint16_t)(cmd32 & 0xFFFFu);
    cmd16 |= (PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);
    cmd16 &= (uint16_t)~PCI_CMD_INTX_DISABLE;
    pci_write_config_dword(pdev->bus, pdev->dev, pdev->fn, 0x04, (cmd32 & 0xFFFF0000u) | cmd16);

    if (yukon_read_hw_mac(g_yukon.mac) == 0)
        g_yukon.native_mac_ok = 1;
    if (yukon_setup_native_tx() != 0)
        g_yukon.native_tx_ok = 0;
    if (yukon_setup_native_status() == 0) {
        if (yukon_setup_native_rx() != 0)
            g_yukon.native_rx_ok = 0;
    }

    if (g_yukon.native_mac_ok) {
        g_yukon_driver.mac.b[0] = g_yukon.mac[0];
        g_yukon_driver.mac.b[1] = g_yukon.mac[1];
        g_yukon_driver.mac.b[2] = g_yukon.mac[2];
        g_yukon_driver.mac.b[3] = g_yukon.mac[3];
        g_yukon_driver.mac.b[4] = g_yukon.mac[4];
        g_yukon_driver.mac.b[5] = g_yukon.mac[5];
    } else if (g_prev_nic) {
        g_yukon_driver.mac = g_prev_nic->mac;
    } else {
        memset(&g_yukon_driver.mac, 0, sizeof(g_yukon_driver.mac));
    }
    register_netdev(&g_yukon_driver);
    g_yukon.attached = 1;

    printk("[yukon mod] attached ");
    printk_hex(pdev->vendor_id);
    printk(":");
    printk_hex(pdev->device_id);
    printk(" bus=");
    printk_hex(pdev->bus);
    printk(" dev=");
    printk_hex(pdev->dev);
    printk(" fn=");
    printk_hex(pdev->fn);
    printk(" mmio=");
    printk_hex(g_yukon.mmio_base);
    if (g_yukon.native_mac_ok) {
        printk(" mac=");
        printk_hex(g_yukon.mac[0]); printk(":");
        printk_hex(g_yukon.mac[1]); printk(":");
        printk_hex(g_yukon.mac[2]); printk(":");
        printk_hex(g_yukon.mac[3]); printk(":");
        printk_hex(g_yukon.mac[4]); printk(":");
        printk_hex(g_yukon.mac[5]);
    }
    printk("\n");
    if (g_yukon.native_tx_ok && g_yukon.native_rx_ok)
        printk("4" "yukon (kmod): experimental native TX/RX polling mode");
    else if (g_yukon.native_tx_ok && g_prev_nic)
        printk("4" "yukon (kmod): native-TX + passthrough/loopback RX mode");
    else if (g_yukon.native_tx_ok)
        printk("4" "yukon (kmod): native-TX + standalone loopback RX mode");
    else if (g_prev_nic)
        printk("4" "yukon (kmod): passthrough+loopback mode; native Yukon RX/TX still TODO");
    else
        printk("4" "yukon (kmod): standalone loopback mode; native Yukon RX/TX still TODO");
    return 0;
}

void pci_driver_remove(pci_device_t *pdev) {
    (void)pdev;
    yukon_detach();
}
