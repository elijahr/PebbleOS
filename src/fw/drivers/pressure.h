/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Initialize the pressure sensor driver. Call this once at startup.
void pressure_init(void);

//! Read a compensated sample from the pressure sensor.
//! @param pressure_pa   [out] barometric pressure in pascals
//! @param temperature_c [out] temperature in centidegrees Celsius (2508 == 25.08 degC)
//! @return true on success, false if the sensor is absent or the read failed.
//! Not every pressure driver implements a read path; drivers that only probe the
//! chip id return false.
bool pressure_read(int32_t *pressure_pa, int32_t *temperature_c);
