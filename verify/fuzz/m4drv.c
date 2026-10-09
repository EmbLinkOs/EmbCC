/* Runs the program's main (renamed sw_main) on the M4 harness and exits
 * QEMU through semihosting SYS_EXIT_EXTENDED with its return value. */
int sw_main(void);
static volatile unsigned blk[2];
int main(void)
{
    int r = sw_main();
    blk[0] = 0x20026u;              /* ADP_Stopped_ApplicationExit */
    blk[1] = (unsigned)r;
    __asm__ volatile("mov r1, %0\n\tmovs r0, #0x20\n\tbkpt #0xab"
                     : : "r"(blk) : "r0", "r1", "memory");
    for (;;) ;
}
