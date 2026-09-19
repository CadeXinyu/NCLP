#ifndef NCLP_MAIN_IMPEDANCE_CANCEL_H
#define NCLP_MAIN_IMPEDANCE_CANCEL_H

/* A53-0 application hooks used by the impedance engine to service the
 * generation-bound cancellation mailbox. */
int nclp_impedance_cancel_requested(void);
void nclp_impedance_cancel_acknowledge(void);

#endif /* NCLP_MAIN_IMPEDANCE_CANCEL_H */
