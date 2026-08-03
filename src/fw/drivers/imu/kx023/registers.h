/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

// Kionix KX023-1025 register map (subset used by the polled driver).

#define KX023_XOUT_L   0x06U
#define KX023_XOUT_H   0x07U
#define KX023_YOUT_L   0x08U
#define KX023_YOUT_H   0x09U
#define KX023_ZOUT_L   0x0AU
#define KX023_ZOUT_H   0x0BU
#define KX023_COTR     0x0CU
#define KX023_WHO_AM_I 0x0FU
#define KX023_CNTL1    0x18U
#define KX023_CNTL2    0x19U
#define KX023_CNTL3    0x1AU
#define KX023_ODCNTL   0x1BU

// WHO_AM_I value for the KX023-1025.
#define KX023_WHO_AM_I_VAL 0x15U

// CNTL1 fields.
#define KX023_CNTL1_PC1  (1U << 7U)  // operating mode (1) vs standby (0)
#define KX023_CNTL1_RES  (1U << 6U)  // high-resolution mode
#define KX023_CNTL1_GSEL_2G (0U << 3U)
#define KX023_CNTL1_GSEL_4G (1U << 3U)
#define KX023_CNTL1_GSEL_8G (2U << 3U)

// CNTL2 fields.
#define KX023_CNTL2_SRST (1U << 7U)  // software reset

// ODCNTL output data rate (OSA[3:0]).
#define KX023_ODCNTL_OSA_12HZ5 0x00U
#define KX023_ODCNTL_OSA_25HZ  0x01U
#define KX023_ODCNTL_OSA_50HZ  0x02U
#define KX023_ODCNTL_OSA_100HZ 0x03U
#define KX023_ODCNTL_OSA_200HZ 0x04U

// 16-bit two's-complement full-scale range (counts) used for mg conversion.
#define KX023_S16_SCALE_RANGE (1U << 15U)
