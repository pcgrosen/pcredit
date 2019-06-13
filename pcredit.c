#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <pci/pci.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/errno.h>

/*
 * Generic PCI configuration space registers.
 */
#define REG_VENDOR          0x00
#define REG_DEVICE          0x04

/*
 * D31:F1 configuration space registers.
 */
#define REG_P2SB_BAR        0x10
#define REG_P2SB_BARH       0x14
#define REG_P2SB_CTRL       0xe0

#define REG_P2SB_CTRL_HIDE  0x0100

/*
 * P2SB private registers.
 */
#define P2SB_PORTID_SHIFT   16

/*
 * Helper functions.
 */

#define MSG(...) do { \
    fprintf(stderr, "[*] " __VA_ARGS__); fprintf(stderr, "\n"); \
  } while(0)
#define ERR(...) do { \
    fprintf(stderr, "[-] " __VA_ARGS__); fprintf(stderr, "\n"); \
    return 1; \
  } while(0)
#define DIE(...) do { *fatal = 1; ERR(__VA_ARGS__) } while(0)

struct pci_dev *pci_find_dev(struct pci_access *pci, uint8_t bus, uint8_t dev, uint8_t func) {
  for(struct pci_dev *it = pci->devices; it; it = it->next) {
    if(it->bus == bus && it->dev == dev && it->func == func) return it;
  }
  return NULL;
}

int get_pch_sbreg_addr(struct pci_access *pci, pciaddr_t *sbreg_addr) {
  MSG("Checking for a Series 10 PCH system");

  struct pci_dev *d31f1 = pci_get_dev(pci, 0, 0, 31, 1);
  pci_fill_info(d31f1, PCI_FILL_IDENT);
  if(d31f1->vendor_id == 0xffff) {
    MSG("Cannot find D31:F1, assuming it is hidden by firmware");

    uint32_t p2sb_ctrl = pci_read_long(d31f1, REG_P2SB_CTRL);
    MSG("P2SB_CTRL=%02x", p2sb_ctrl);
    if(!(p2sb_ctrl & REG_P2SB_CTRL_HIDE)) {
      ERR("D31:F1 is hidden but P2SB_E1 is not 0xff, bailing out");
    }

    MSG("Unhiding P2SB");
    pci_write_long(d31f1, REG_P2SB_CTRL, p2sb_ctrl & ~REG_P2SB_CTRL_HIDE);

    p2sb_ctrl = pci_read_long(d31f1, REG_P2SB_CTRL);
    MSG("P2SB_CTRL=%02x", p2sb_ctrl);
    if(p2sb_ctrl & REG_P2SB_CTRL_HIDE) {
      ERR("Cannot unhide PS2B");
    }

    pci_fill_info(d31f1, PCI_FILL_RESCAN | PCI_FILL_IDENT);
    if(d31f1->vendor_id == 0xffff) {
      ERR("P2SB unhidden but does not enumerate, bailing out");
    }
  }

  pci_fill_info(d31f1, PCI_FILL_RESCAN | PCI_FILL_IDENT | PCI_FILL_BASES);
  if(d31f1->vendor_id != 0x8086) {
    ERR("Vendor of D31:F1 is not Intel");
  } else if((uint32_t)d31f1->base_addr[0] == 0xffffffff) {
    ERR("SBREG_BAR is not implemented in D31:F1");
  }

  *sbreg_addr = d31f1->base_addr[0] &~ 0xf;
  MSG("SBREG_ADDR=%08lx", *sbreg_addr);

  MSG("Hiding P2SB again");
  uint32_t p2sb_ctrl = pci_read_long(d31f1, REG_P2SB_CTRL);
  pci_write_long(d31f1, REG_P2SB_CTRL, p2sb_ctrl | REG_P2SB_CTRL_HIDE);

  pci_fill_info(d31f1, PCI_FILL_RESCAN | PCI_FILL_IDENT);
  if(d31f1->vendor_id != 0xffff) {
    ERR("Cannot hide P2SB");
  }

  return 0;
}

uint32_t sideband_read(void *sbmap, uint8_t port, uint16_t reg) {
  volatile uint32_t *addr;
  uint32_t val;
  addr = (volatile uint32_t *)((uintptr_t)sbmap + (port << P2SB_PORTID_SHIFT) + reg);
  val = *addr;
  MSG("*%p == %08x", addr, val);
  return val;
}

void sideband_write(void *sbmap, uint8_t port, uint16_t reg, uint32_t value) {
  volatile uint32_t *addr;
  addr = (volatile uint32_t *)((uintptr_t)sbmap + (port << P2SB_PORTID_SHIFT) + reg);
  MSG("*%p =  %08x", addr, value);
  *addr = value;
}

int try_pch(struct pci_access *pci, uint8_t port, uint32_t offset, uint8_t do_write, uint32_t value) {
  pciaddr_t sbreg_addr;
  if(get_pch_sbreg_addr(pci, &sbreg_addr)) {
    MSG("Re-enumerating PCI devices will probably crash the system");
    ERR("Probing Series 100 PCH failed");
  }

  int memfd = open("/dev/mem", O_RDWR);
  if(memfd == -1) {
    ERR("Cannot open /dev/mem");
  }

  void *sbmap = mmap((void*)sbreg_addr, 1<<24, PROT_READ|PROT_WRITE, MAP_SHARED,
                     memfd, sbreg_addr);
  if(sbmap == MAP_FAILED) {
    if(errno == EPERM) {
      // The requirement might be relaxed to CONFIG_IO_DEVMEM_STRICT=n, but I'm not sure.
      MSG("Is your kernel configured with CONFIG_DEVMEM_STRICT=n?");
    }
    ERR("Cannot map SBREG");
  }

  close(memfd);

  sideband_read(sbmap, port, offset);

  if (do_write) {
    sideband_write(sbmap, port, offset, value);

    sideband_read(sbmap, port, offset);
  }

  return 0;
}

int create_pci(int method, struct pci_access **pci_out)  {
  struct pci_access *pci = pci_alloc();
  pci->method = method;
  pci_init(pci);
  pci_scan_bus(pci);

  struct pci_dev *d31f0 = pci_find_dev(pci, 0, 31, 0);
  if(!d31f0) {
    ERR("Cannot find D31:F0");
  }

  pci_fill_info(d31f0, PCI_FILL_IDENT | PCI_FILL_BASES);
  if(d31f0->vendor_id != 0x8086) {
    ERR("Vendor of D31:F0 is not Intel");
  }

  *pci_out = pci;
  return 0;
}

int main(int argc, char **argv) {
  struct pci_access *pci;
  uint32_t port;
  uint32_t offset;
  uint8_t do_write = 0;
  uint32_t value = 0;

  if (argc != 3 && argc != 4) {
    printf("Usage: %s <port> <register offset (in bytes)> [value to write]\n", argv[0]);
    return 1;
  }

  port = strtoul(argv[1], NULL, 16);
  offset = strtoul(argv[2], NULL, 16);

  if (argc > 3) {
    do_write = 1;
    value = strtoul(argv[3], NULL, 16);
  }

  if(create_pci(PCI_ACCESS_AUTO, &pci)) {
    MSG("Is this an Intel platform?");
    return 1;
  }

  if(create_pci(PCI_ACCESS_I386_TYPE1, &pci)) {
    return 1;
  }

  if(try_pch(pci, port, offset, do_write, value)) {
    return 1;
  }

  printf("[+] Done\n");
  return 0;
}
