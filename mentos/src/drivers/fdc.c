/// @file fdc.c
/// @brief Floppy driver controller handling.
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.
/// @addtogroup fdc
/// @{

// Setup the logging for this file (do this before any other include).
#include "sys/kernel_levels.h"           // Include kernel log levels.
#define __DEBUG_HEADER__ "[FDC   ]"      ///< Change header.
#define __DEBUG_LEVEL__  LOGLEVEL_NOTICE ///< Set log level.
#include "io/debug.h"                    // Include debugging functions.

#include "drivers/fdc.h"
#include "io/port_io.h"
#include "io/video.h"

/// @brief Floppy Disk Controller (FDC) registers.
typedef enum fdc_registers {
    STATUS_REGISTER_A              = 0x3F0,
    ///< This register is read-only and monitors the state of the
    ///< interrupt pin and several disk interface pins.
    STATUS_REGISTER_B              = 0x3F1,
    ///< This register is read-only and monitors the state of several
    ///< isk interface pins.
    DOR                            = 0x3F2,
    ///< The Digital Output Register contains the drive select and
    ///< motor enable bits, a reset bit and a DMA GATE bit.
    TAPE_DRIVE_REGISTER            = 0x3F3,
    ///< This register allows the user to assign tape support to a
    ///< particular drive during initialization.
    MAIN_STATUS_REGISTER           = 0x3F4,
    ///< The Main Status Register is a read-only register and is used
    ///< for controlling command input and result output for all commands.
    DATARATE_SELECT_REGISTER       = 0x3F4,
    ///< This register is included for compatibility with the 82072
    ///< floppy controller and is write-only.
    DATA_FIFO                      = 0x3F5,
    ///< All command parameter information and disk data transfers go
    ///< through the FIFO.
    DIGITAL_INPUT_REGISTER         = 0x3F7,
    ///< This register is read only in all modes.
    CONFIGURATION_CONTROL_REGISTER = 0x3F7
    ///< This register sets the datarate and is write only.
} fdc_registers_t;

int fdc_initialize(void)
{
    /* Setting bits:
     * 2: (RESET)
     * 3: (IRQ)
     */
    outportb(DOR, 0x0c);
    return 0;
}

int fdc_finalize(void)
{
    /* Setting bits:
     * 3: (IRQ)
     * 4: (MOTA)
     */
    outportb(DOR, 0x18);
    return 0;
}

/// @}
