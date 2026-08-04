/*
 * SPDX-FileCopyrightText: 2026 Core Devices LLC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone linker script for the nRF52840 bootloader image.
 *
 * The bootloader is a separate ELF linked at flash 0x0, size <= 32 KiB. It
 * does NOT share the firmware memory.ld (that fragment carves the SRAM for the
 * kernel/worker/app and asserts a firmware-only layout).
 *
 * RAM starts at 0x20000100 (NOT 0x20000000) to stay clear of the 256-byte
 * retained page at 0x20000000..0x200000FF. The bootloader must not clobber the
 * boot bits / retained CRC that the firmware reads. This is load-bearing.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x8000      /* 32 KiB */
    RAM   (xrw) : ORIGIN = 0x20000100, LENGTH = 0x0003FF00  /* below retained page */
}

ENTRY(Reset_Handler)

SECTIONS
{
    /* Vector table must sit at the very start of flash: reset fetches the
     * initial SP and PC from here. */
    .isr_vector :
    {
        . = ALIGN(4);
        PROVIDE(__ISR_VECTOR_TABLE__ = .);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH
    ASSERT(__ISR_VECTOR_TABLE__ == ORIGIN(FLASH),
           "boot: vector table is not at the start of flash")

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        . = ALIGN(4);
        __etext = .;
    } >FLASH

    /* Initialized data: resides in RAM at run time, loaded from flash. */
    .data : AT (__etext)
    {
        . = ALIGN(4);
        __data_start = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        __data_end = .;
    } >RAM
    __data_load_start = LOADADDR(.data);

    /* Zero-initialized data. NOLOAD: consumes no flash. */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } >RAM

    /* Top-of-stack for the reset vector's first word. */
    _estack = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ :
    {
        *(.ARM.exidx)
        *(.ARM.exidx*)
        *(.ARM.extab*)
    }

    /* Everything the image occupies in flash must fit in 32 KiB. */
    ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(FLASH) + LENGTH(FLASH),
           "boot: image does not fit in 32 KiB of flash")
}
