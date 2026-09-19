/* A53-0 is the sole runtime UART owner.  Keeping A53-1 silent prevents lwIP
 * startup and command logs from interleaving with headstage diagnostics.
 * Set NCLP_NETWORK_UART=1 only for low-level Ethernet bring-up. */

#ifndef NCLP_NETWORK_UART
#define NCLP_NETWORK_UART 0
#endif

#if NCLP_NETWORK_UART == 0
void __wrap_outbyte(char character);

void __wrap_outbyte(char character)
{
    (void)character;
}
#endif
