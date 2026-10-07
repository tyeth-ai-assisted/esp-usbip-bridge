#ifndef SCPI_CONTROLLER_H
#define SCPI_CONTROLLER_H

#include "scpi/scpi.h"

/* HUB:*, USB:* and MUX:* commands appended to the virtual harness SCPI table. */
const scpi_command_t *scpi_controller_commands(void);

#endif
