/// @file stdbool.h
/// @brief Defines the boolean values.
/// @copyright (c) 2014-2024 This file is distributed under the MIT License.
/// See LICENSE.md for details.

#pragma once

/// @brief Define boolean value.
typedef enum bool {
    false, ///< [0] False.
    true   ///< [1] True.
} __attribute__((__packed__)) bool_t;
