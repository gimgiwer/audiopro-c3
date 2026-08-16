#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

void dump_range(int fd, uint32_t phys_addr, uint32_t count, const char *title) {
    uint32_t page_base = phys_addr & ~0xFFF;
    uint32_t page_offset = phys_addr & 0xFFF;
    
    void *map = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, page_base);
    if (map == MAP_FAILED) {
        printf("[-] Failed to mmap 0x%08X\n", phys_addr);
        return;
    }
    
    printf("\n=== %s (Base: 0x%08X) ===\n", title, phys_addr);
    volatile uint32_t *ptr = (volatile uint32_t *)((char *)map + page_offset);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t val = ptr[i];
        printf("  [0x%08X] = 0x%08X\n", phys_addr + (i * 4), val);
    }
    munmap(map, 0x1000);
}

int main(void) {
    int fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }
    
    printf("====================================================\n");
    printf("🔊 AUDIO PRO C3 / MT7628 HARDWARE REGISTER DUMP\n");
    printf("====================================================\n");
    
    // System control / Pinmux (0x10000000)
    dump_range(fd, 0x10000000, 4, "CHIP_ID & SYSTEM_STATUS");
    dump_range(fd, 0x1000002C, 4, "CLKCFG (Clock configuration)");
    dump_range(fd, 0x10000060, 2, "AGPIO_CFG / GPIO_MODE (Pinmux)");
    
    // I2S Controller (0x10000A00)
    dump_range(fd, 0x10000A00, 10, "I2S CONTROLLER REGISTERS (CFG0, CFG1, DIV, etc)");
    
    // GDMA Controller (0x10002800)
    dump_range(fd, 0x10002800, 8, "GDMA CONTROLLER REGISTERS");
    
    close(fd);
    return 0;
}
