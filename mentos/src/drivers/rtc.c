/// @file   rtc.c
/// @brief  Real Time Clock (RTC) driver.
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.
/// @addtogroup rtc
/// @{

// Setup the logging for this file (do this before any other include).
#include "sys/kernel_levels.h"           // Include kernel log levels.
#define __DEBUG_HEADER__ "[RTC   ]"      ///< Change header.
#define __DEBUG_LEVEL__  LOGLEVEL_NOTICE ///< Set log level.
#include "io/debug.h"                    // Include debugging functions.

#include "descriptor_tables/isr.h"
#include "drivers/rtc.h"
#include "hardware/pic8259.h"
#include "io/port_io.h"
#include "kernel.h"
#include "string.h"

#define CMOS_ADDR 0x70 ///< Addess where we need to write the Address.
#define CMOS_DATA 0x71 ///< Addess where we need to write the Data.

/// Current global time.
tm_t global_time;
/// Previous global time.
tm_t previous_global_time;
/// Data type is BCD.
int is_bcd;

/// @brief Checks if the two time values are different.
/// @param t0 the first time value.
/// @param t1 the second time value.
/// @return 1 if they are different, 0 otherwise.
static inline unsigned int rtc_are_different(tm_t *t0, tm_t *t1)
{
    if (t0->tm_sec != t1->tm_sec) {
        return 1;
    }
    if (t0->tm_min != t1->tm_min) {
        return 1;
    }
    if (t0->tm_hour != t1->tm_hour) {
        return 1;
    }
    if (t0->tm_mon != t1->tm_mon) {
        return 1;
    }
    if (t0->tm_year != t1->tm_year) {
        return 1;
    }
    if (t0->tm_wday != t1->tm_wday) {
        return 1;
    }
    if (t0->tm_mday != t1->tm_mday) {
        return 1;
    }
    return 0;
}

/// @brief Check if rtc is updating time currently.
/// @return 1 if RTC is updating, 0 otherwise.
static inline unsigned int is_updating_rtc(void)
{
    outportb(CMOS_ADDR, 0x0A);
    uint32_t status = inportb(CMOS_DATA);
    return (status & 0x80U) != 0;
}

/// @brief Reads the given register.
/// @param reg the register to read.
/// @return the value we read.
static inline unsigned char read_register(unsigned char reg)
{
    outportb(CMOS_ADDR, reg);
    return inportb(CMOS_DATA);
}

/// @brief Writes on the given register.
/// @param reg the register on which we need to write.
/// @param value the value we want to write.
static inline void write_register(unsigned char reg, unsigned char value)
{
    outportb(CMOS_ADDR, reg);
    outportb(CMOS_DATA, value);
}

/// @brief Transforms a Binary-Coded Decimal (BCD) to decimal.
/// @param bcd the BCD value.
/// @return the decimal value.
static inline unsigned char bcd2bin(unsigned char bcd) { return ((bcd >> 4U) * 10) + (bcd & 0x0FU); }

/// @brief Reads the current datetime value from a real-time clock.
static inline void rtc_read_datetime(void)
{
    if (read_register(0x0CU) & 0x10U) {
        if (is_bcd) {
            global_time.tm_sec  = bcd2bin(read_register(0x00));
            global_time.tm_min  = bcd2bin(read_register(0x02));
            global_time.tm_hour = bcd2bin(read_register(0x04)) + 2;
            global_time.tm_mon  = bcd2bin(read_register(0x08));
            global_time.tm_year = bcd2bin(read_register(0x09)) + 2000;
            global_time.tm_wday = bcd2bin(read_register(0x06));
            global_time.tm_mday = bcd2bin(read_register(0x07));
        } else {
            global_time.tm_sec  = read_register(0x00);
            global_time.tm_min  = read_register(0x02);
            global_time.tm_hour = read_register(0x04) + 2;
            global_time.tm_mon  = read_register(0x08);
            global_time.tm_year = read_register(0x09) + 2000;
            global_time.tm_wday = read_register(0x06);
            global_time.tm_mday = read_register(0x07);
        }
    }
}

/// @brief Updates the internal datetime value.
static inline void rtc_update_datetime(void)
{
    static unsigned int first_update = 1;
    // Wait until rtc is not updating.
    while (is_updating_rtc()) {
    }
    // Read the values.
    rtc_read_datetime();
    if (first_update) {
        do {
            // Save the previous global time.
            previous_global_time = global_time;
            // Wait until rtc is not updating.
            while (is_updating_rtc()) {
            }
            // Read the values.
            rtc_read_datetime();
        } while (!rtc_are_different(&previous_global_time, &global_time));
        first_update = 0;
    }
}

/// @brief Callback for RTC.
/// @param f the current registers.
static inline void rtc_handler_isr(pt_regs_t *f) { rtc_update_datetime(); }

void gettime(tm_t *time)
{
    // Copy the update time.
    memcpy(time, &global_time, sizeof(tm_t));
}

int rtc_initialize(void)
{
    unsigned char status;

    status = read_register(0x0B);
    status |= 0x02U;            // 24 hour clock
    status |= 0x10U;            // update ended interrupts
    status &= ~0x20U;           // no alarm interrupts
    status &= ~0x40U;           // no periodic interrupt
    is_bcd = !(status & 0x04U); // check if data type is BCD
    write_register(0x0B, status);

    read_register(0x0C);

    // Install the IRQ.
    irq_install_handler(IRQ_REAL_TIME_CLOCK, rtc_handler_isr, "Real Time Clock (RTC)");
    // Enable the IRQ.
    pic8259_irq_enable(IRQ_REAL_TIME_CLOCK);
    // Wait until rtc is ready.
    rtc_update_datetime();
    return 0;
}

int rtc_finalize(void)
{
    // Uninstall the IRQ.
    irq_uninstall_handler(IRQ_REAL_TIME_CLOCK, rtc_handler_isr);
    // Disable the IRQ.
    pic8259_irq_disable(IRQ_REAL_TIME_CLOCK);
    return 0;
}

/// @}
