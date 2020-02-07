// The local APIC manages internal (non-I/O) interrupts.
// See Chapter 8 & Appendix C of Intel processor manual volume 3.

#include <inc/types.h>

#include <arch/i386/memlayout.h>
#include <arch/i386/traps.h>
#include <arch/i386/mmu.h>
#include <arch/i386/x86.h>

#include <kern/console.h>

// Local APIC registers, divided by 4 for use as uint[] indices.
#define ID      (0x0020/4)   // ID
#define VER     (0x0030/4)   // Version
    #define VER_NO(ver_reg) (ver_reg&0xFF) // 0x1x for 82489DX, 0x10~0x15 for Intergrated APIC
    #define VER_MAX_LVT_ENTRY(ver_reg) (((ver_reg>>16)&0xFF)+1)
#define TPR     (0x0080/4)   // Task Priority
#define EOI     (0x00B0/4)   // EOI
#define SVR     (0x00F0/4)   // Spurious Interrupt Vector
  #define ENABLE     0x00000100   // Unit Enable
#define ESR     (0x0280/4)   // Error Status
#define ICRLO   (0x0300/4)   // Interrupt Command
  #define INIT       0x00000500   // INIT/RESET
  #define STARTUP    0x00000600   // Startup IPI
  #define DELIVS     0x00001000   // Delivery status
  #define ASSERT     0x00004000   // Assert interrupt (vs deassert)
  #define DEASSERT   0x00000000
  #define LEVEL      0x00008000   // Level triggered
  #define BCAST      0x00080000   // Send to all APICs, including self.
  #define BUSY       0x00001000
  #define FIXED      0x00000000
#define ICRHI   (0x0310/4)   // Interrupt Command [63:32]
#define TIMER   (0x0320/4)   // Local Vector Table 0 (TIMER)
  #define X1         0x0000000B   // divide counts by 1
  #define PERIODIC   0x00020000   // Periodic
#define PCINT   (0x0340/4)   // Performance Counter LVT
#define LINT0   (0x0350/4)   // Local Vector Table 1 (LINT0)
#define LINT1   (0x0360/4)   // Local Vector Table 2 (LINT1)
#define ERROR   (0x0370/4)   // Local Vector Table 3 (ERROR)
  #define MASKED     0x00010000   // Interrupt masked
#define TICR    (0x0380/4)   // Timer Initial Count
#define TCCR    (0x0390/4)   // Timer Current Count
#define TDCR    (0x03E0/4)   // Timer Divide Configuration

volatile uint32_t *lapic;  // Initialized in mp.c

static void
lapicw(int index, int value)
{
    lapic[index] = value;
    lapic[ID];  // wait for write to finish, by reading
}

void
lapic_init(void)
{
    if(!lapic)
        panic("lapic not initialized.\n");
        
    //cprintf("lapic version: 0x%x\n", VER_NO(lapic[VER]));
    //cprintf("lapic MAX LVT Entry: 0x%x\n", VER_MAX_LVT_ENTRY(lapic[VER]));

    // Enable local APIC; set spurious interrupt vector.
    lapicw(SVR, ENABLE | (T_IRQ0 + IRQ_SPURIOUS));

    // The timer repeatedly counts down at bus frequency
    // from lapic[TICR] and then issues an interrupt.
    // If xv6 cared more about precise timekeeping,
    // TICR would be calibrated using an external time source.
    lapicw(TDCR, X1);
    lapicw(TIMER, PERIODIC | (T_IRQ0 + IRQ_TIMER));
    lapicw(TICR, 10000000);

    // Disable logical interrupt lines.
    lapicw(LINT0, MASKED);
    lapicw(LINT1, MASKED);

    // Disable performance counter overflow interrupts
    // on machines that provide that interrupt entry.
    if(((lapic[VER]>>16) & 0xFF) >= 4)
        lapicw(PCINT, MASKED);

    // Map error interrupt to IRQ_ERROR.
    lapicw(ERROR, T_IRQ0 + IRQ_ERROR);

    // Clear error status register (requires back-to-back writes).
    lapicw(ESR, 0);
    lapicw(ESR, 0);

    // Ack any outstanding interrupts.
    lapicw(EOI, 0);

    // Send an Init Level De-Assert to synchronise arbitration ID's.
    lapicw(ICRHI, 0);
    lapicw(ICRLO, BCAST | INIT | LEVEL);
    while(lapic[ICRLO] & DELIVS)
        ;

    // Enable interrupts on the APIC (but not on the processor).
    lapicw(TPR, 0);
}

int
lapicid(void)
{
    if (!lapic) 
        panic("lapic not initialized.\n");
    return lapic[ID] >> 24;
}

// Acknowledge interrupt.
void
lapic_eoi(void)
{
    if(lapic)
        lapicw(EOI, 0);
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
void
micro_delay(int us)
{
    for (int i = 0; i < us; i ++) {
        int x = 0xffff;
        while(x--);
    }
}

#define CMOS_PORT    0x70
#define CMOS_RETURN  0x71

// Start additional processor running entry code at addr.
// See Appendix B of MultiProcessor Specification.
void
lapic_startap(uint8_t apicid, uint32_t addr)
{
    int i;
    uint16_t *wrv;

    // "The BSP must initialize CMOS shutdown code to 0AH
    // and the warm reset vector (DWORD based at 40:67) to point at
    // the AP startup code prior to the [universal startup algorithm]."
    outb(CMOS_PORT, 0xF);  // offset 0xF is shutdown code
    outb(CMOS_PORT+1, 0x0A);
    wrv = (ushort*)P2V((0x40<<4 | 0x67));  // Warm reset vector
    wrv[0] = 0;
    wrv[1] = addr >> 4;

    // "Universal startup algorithm."
    // Send INIT (level-triggered) interrupt to reset other CPU.
    lapicw(ICRHI, apicid<<24);
    lapicw(ICRLO, INIT | LEVEL | ASSERT);
    micro_delay(200);
    lapicw(ICRLO, INIT | LEVEL);
    micro_delay(100);    // should be 10ms, but too slow in Bochs!

    // Send startup IPI (twice!) to enter code.
    // Regular hardware is supposed to only accept a STARTUP
    // when it is in the halted state due to an INIT.  So the second
    // should be ignored, but it is part of the official Intel algorithm.
    // Bochs complains about the second one.  Too bad for Bochs.
    for(i = 0; i < 2; i ++) {
        lapicw(ICRHI, apicid<<24);
        lapicw(ICRLO, STARTUP | (addr>>12));
        micro_delay(200);
    }
}