/* startup.c — minimal reset vector for STM32G474RE. No CMSIS. */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata) *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; ) *dst++ = 0;
    main();
    for (;;) { }
}

static void Default_Handler(void) { for (;;) { } }

/* Only the ARM core vectors are populated; no peripheral interrupts are used. */
__attribute__((section(".isr_vector"), used))
void (* const g_vectors[])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    Default_Handler,   /* NMI        */
    Default_Handler,   /* HardFault  */
    Default_Handler,   /* MemManage  */
    Default_Handler,   /* BusFault   */
    Default_Handler,   /* UsageFault */
    0, 0, 0, 0,
    Default_Handler,   /* SVCall     */
    Default_Handler,   /* DebugMon   */
    0,
    Default_Handler,   /* PendSV     */
    Default_Handler,   /* SysTick    */
};
