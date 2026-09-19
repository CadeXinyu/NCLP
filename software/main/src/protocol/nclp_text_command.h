#ifndef NCLP_TEXT_COMMAND_H
#define NCLP_TEXT_COMMAND_H

#include "nclp_main.h"

/* Parse one NUL-terminated command without its TCP newline/CR terminator.
 * Returns zero on success, -1 for an unknown command or invalid arguments.
 * Output is valid only on success. This module has no hardware or lwIP state.
 */
int nclp_command_parse_text(const char *text, nclp_main_command_t *command);

#endif
