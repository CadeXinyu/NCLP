#ifndef NCLP_MAIN_LED_CONTROL_H
#define NCLP_MAIN_LED_CONTROL_H

#include <stdint.h>

/* Initialize the PL LED block with every software-controlled light off. */
int nclp_led_init(void);

/*
 * Update the four port-presence lights and the independent fault light.
 * physical_chip_mask uses A1,A2,B1,B2,C1,C2,D1,D2 in bits [7:0].
 */
void nclp_led_update(uint8_t physical_chip_mask, uint32_t init_valid,
                     uint32_t fault);

#endif /* NCLP_MAIN_LED_CONTROL_H */
