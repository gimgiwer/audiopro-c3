/* Tiny direct-syscall MIPS32 O32 register dumper (zero dependencies, ~800 bytes) */
typedef unsigned int uint32_t;

#define SYS_exit   4001
#define SYS_read   4003
#define SYS_write  4004
#define SYS_open   4005
#define SYS_close  4006
#define SYS_munmap 4091
#define SYS_mmap2  4210

#define O_RDONLY 0
#define PROT_READ 1
#define MAP_SHARED 1

static long syscall3(long num, long a1, long a2, long a3) {
    register long v0 __asm__("$2") = num;
    register long a0 __asm__("$4") = a1;
    register long a1_ __asm__("$5") = a2;
    register long a2_ __asm__("$6") = a3;
    __asm__ volatile("syscall" : "+r"(v0), "+r"(a0), "+r"(a1_), "+r"(a2_) : : "$1", "$3", "$7", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "memory");
    return v0;
}

static long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    register long v0 __asm__("$2") = num;
    register long a0 __asm__("$4") = a1;
    register long a1_ __asm__("$5") = a2;
    register long a2_ __asm__("$6") = a3;
    register long a3_ __asm__("$7") = a4;
    long sp_args[2] = {a5, a6};
    __asm__ volatile(
        "subu $sp, $sp, 32\n"
        "lw $8, 0(%4)\n"
        "sw $8, 16($sp)\n"
        "lw $8, 4(%4)\n"
        "sw $8, 20($sp)\n"
        "syscall\n"
        "addu $sp, $sp, 32\n"
        : "+r"(v0), "+r"(a0), "+r"(a1_), "+r"(a2_), "+r"(a3_)
        : "r"(sp_args)
        : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "memory"
    );
    return v0;
}

static void print_str(const char *s) {
    long len = 0;
    while (s[len]) len++;
    syscall3(SYS_write, 1, (long)s, len);
}

static void print_hex32(uint32_t val) {
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    const char hex[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[10] = 0;
    print_str(buf);
}

static void dump_reg(int fd, uint32_t addr, const char *label) {
    uint32_t page_base = addr & ~0xFFF;
    uint32_t page_off  = addr & 0xFFF;
    
    long map = syscall6(SYS_mmap2, 0, 4096, PROT_READ, MAP_SHARED, fd, page_base >> 12);
    if (map < 0 && map > -4096) {
        print_str("[-] Error mmap for ");
        print_str(label);
        print_str("\n");
        return;
    }
    
    volatile uint32_t *ptr = (volatile uint32_t *)((char *)map + page_off);
    uint32_t val = *ptr;
    
    print_str("  [");
    print_hex32(addr);
    print_str("] = ");
    print_hex32(val);
    print_str(" (");
    print_str(label);
    print_str(")\n");
    
    syscall3(SYS_munmap, map, 4096, 0);
}

void _start(void) {
    int fd = syscall3(SYS_open, (long)"/dev/mem", O_RDONLY, 0);
    if (fd < 0) {
        print_str("[-] Error: cannot open /dev/mem\n");
        syscall3(SYS_exit, 1, 0, 0);
    }
    
    print_str("\n========================================================\n");
    print_str("AUDIO PRO C3 (MT7628) REAL-TIME HARDWARE REGISTER DUMP\n");
    print_str("========================================================\n");
    
    print_str("--- 1. PINMUX & CLOCK ---\n");
    dump_reg(fd, 0x10000000, "CHIP_ID");
    dump_reg(fd, 0x1000002C, "CLKCFG (Clock gating / dividers)");
    dump_reg(fd, 0x10000060, "AGPIO_CFG / GPIO_MODE (Pinmux I2S/UART/etc)");
    
    print_str("\n--- 2. I2S REGISTERS ---\n");
    dump_reg(fd, 0x10000A00, "I2S_REG_CFG0");
    dump_reg(fd, 0x10000A04, "I2S_REG_INT_STATUS");
    dump_reg(fd, 0x10000A08, "I2S_REG_INT_EN");
    dump_reg(fd, 0x10000A0C, "I2S_REG_FF_STATUS");
    dump_reg(fd, 0x10000A10, "I2S_REG_WREG");
    dump_reg(fd, 0x10000A14, "I2S_REG_RREG");
    dump_reg(fd, 0x10000A18, "I2S_REG_CFG1");
    dump_reg(fd, 0x10000A1C, "I2S_REG_DIVINT");
    dump_reg(fd, 0x10000A20, "I2S_REG_DIVCOMP");
    dump_reg(fd, 0x10000A24, "I2S_REG_FLAGS");
    
    print_str("\n--- 3. GDMA AUDIO CONTROLLER ---\n");
    dump_reg(fd, 0x10002800, "GDMA_CTRL");
    dump_reg(fd, 0x10002804, "GDMA_SRC");
    dump_reg(fd, 0x10002808, "GDMA_DST");
    dump_reg(fd, 0x1000280C, "GDMA_COUNT");
    
    print_str("========================================================\n\n");
    
    syscall3(SYS_close, fd, 0, 0);
    syscall3(SYS_exit, 0, 0, 0);
}
