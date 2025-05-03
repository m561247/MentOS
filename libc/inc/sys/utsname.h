/// @file utsname.h
/// @brief Functions used to provide information about the machine & OS.
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.

#pragma once

/// Maximum length of the string used by utsname.
#define SYS_LEN 257

/// @brief Holds information concerning the machine and the os.
typedef struct utsname {
    /// The name of the system.
    char sysname[SYS_LEN];
    /// The name of the node.
    char nodename[SYS_LEN];
    /// Operating system release (e.g., "2.6.28").
    char release[SYS_LEN];
    /// The version of the OS.
    char version[SYS_LEN];
    /// The name of the machine.
    char machine[SYS_LEN];
} utsname_t;

/// @brief Returns system information in the structure pointed to by buf.
/// @param buf Buffer where the info will be placed.
/// @return 0 on success, a negative value on failure.
int uname(utsname_t *buf);
