# nor_read.tcl -- Bangle.js 2 external SPI-NOR, HOST-DRIVEN READ PATH (READ ONLY)
# =============================================================================
#
# PROVENANCE
#   Derived from nor_copytest.tcl (2026-08-01), the write-path script that ran
#   on this bench against this unit and passed its RDID assertion. That script
#   is preserved, unrunnable, alongside this one as
#   nor_copytest.tcl.DANGEROUS-WRITE-PATH.txt. This file is the read half of it
#   with every mutating opcode and every top-level device action removed.
#
#   Full method reference: R10 (bootloader+PRF) design doc, section 12.1
#   "Do the READ path with NO STUB AT ALL. Host-driven SPIM2."
#   ~/.local/spellbook/docs/Users-eek-Development-PebbleOS/plans/
#     2026-07-28-bangle2-r10-bootloader-design.md
#
# NON-MUTATING BY CONSTRUCTION
#   This file can issue exactly FOUR SPI-NOR opcodes:
#     0x9F RDID    read
#     0x05 RDSR1   read
#     0x03 READ    read
#     0xAB release-from-deep-power-down -- THE ONE NON-READ OPCODE HERE.
#   Do not let the audit answer drift back to a flat "none": it is "one
#   non-read opcode, deliberately present, non-mutating". 0xAB neither erases
#   nor programs anything. It is present because without it a part sitting in
#   deep power-down answers RDID with all-0x00 or all-0xFF, which is
#   indistinguishable from a dead bus (bangle2_flash_ids.h classifies both as
#   Bangle2FlashIdDeadBus). That turns a two-second wake into a wrong
#   conclusion about the hardware, at a bench, under pressure. It is exposed as
#   nor_wake and is NEVER called implicitly -- the caller decides. See nor_wake.
#
#   Absent by deliberate exclusion, every one of them mutating:
#     0x01 WRSR, 0x02 PP, 0x04 WRDI, 0x06 WREN, 0x20 SE, 0x52 BE32K,
#     0xD8 BE64K, 0xC7/0x60 CE, 0x66/0x99 reset-enable/reset.
#   (0x04 WRDI only clears the write-enable latch and is harmless in isolation,
#   but it is a write-path opcode and there is no reason for it here.)
#   Critically there is no 0x06 WREN, so the chip's write-enable latch stays
#   clear for the whole session and any mutating opcode would be ignored even
#   if one were somehow clocked out.
#
#   This file DEFINES PROCS AND NOTHING ELSE. Sourcing it is inert: no init,
#   no reset, no halt, no program, no shutdown, no register write happens at
#   source time. Nothing touches the device until the caller invokes a proc.
#
# CONSTRAINTS THAT MAKE IT SAFE -- the caller owns these
#   1. THE CPU MUST BE HALTED for the whole operation ("reset halt", or "halt"
#      on a running target). SPIM2 and the NOR chip-select GPIO are shared with
#      the firmware's own flash driver; if the firmware runs concurrently the
#      two drivers fight over the peripheral and the bus. Halted also means the
#      firmware's software write-protection state machine (spi_nor.c:282-299)
#      is not running and is irrelevant. R10 12.1.
#   2. NO CODE IS PLACED ON THE TARGET. There is no stub, no linker script and
#      no vector table. Everything here is register pokes over SWD. The worst
#      possible outcome of this file is a failed read.
#   3. SCRATCH RAM MUST BE SAVED AND RESTORED BY THE CALLER. spim_setup and
#      spi_txn overwrite TX_BUF and RX_BUF (see below) and reconfigure SPIM2
#      and P0.14. Read those regions out before use and write them back after,
#      so the operation is genuinely non-destructive. Do not assume any RAM
#      region is free -- the build reporting WORKER_RAM/APP_RAM at 0 percent is
#      suggestive, not a guarantee. R10 12.1.
#   4. NEVER PLACE A SCRATCH BUFFER IN 0x20000000-0x200000FF. That is the
#      retained page and it holds the boot bits. nor_read_max below refuses to
#      proceed if either buffer overlaps it; that guard is arithmetic only and
#      touches no hardware.
#
# UNPROVEN CHECK -- ADDED HERE, NOT PROVEN ON HARDWARE. TREAT AS UNDER TEST.
#   spi_txn asserts that SPIM2's RXD.AMOUNT (0x4002353C) equals the RXD.MAXCNT
#   it programmed, and errors loudly if not. Rationale: EVENTS_END fires when
#   the transaction ENDS, which is not the same as it having moved every byte.
#   An undetected short transaction produces a backup that reports success and
#   is silently truncated or misaligned -- the worst possible failure for the
#   one artifact a restore would depend on.
#
#   This assertion did NOT run on the 2026-08-01 bench session. The original
#   nor_copytest.tcl never read RXD.AMOUNT, so nothing here is evidence about
#   what this part reports. THE FIRST BENCH RUN IS ITS FIRST TEST. If it fires,
#   do not assume the read was short and do not assume the check is wrong --
#   read RXD.AMOUNT and RXD.MAXCNT by hand and find out which. Either answer is
#   worth having on read 1 rather than read 100.
#
#   The escape hatch is `set SPIM_CHECK_AMOUNT 0`, and it exists so an unproven
#   check can never be the sole reason a backup cannot be taken. Everything read
#   with it at 0 is unverified against short transactions. Turn it back on.
#
# TYPICAL USE (the caller drives; this file only supplies the verbs)
#     init
#     halt                              ;# or: reset halt
#     # ... save TX_BUF/RX_BUF contents here ...
#     spim_setup
#     echo [nor_rdid]                   ;# FIRST command to issue. R10 12.1.
#     # if RDID is all-0x00 or all-0xFF the part may be in deep power-down,
#     # not dead:  nor_wake ; echo [nor_rdid]
#     nor_assert_idle                   ;# WIP must be clear before trusting data
#     set bytes [nor_read 0x000000 256]
#     # ... restore TX_BUF/RX_BUF contents here ...
#
#   For a BULK BACKUP do not call nor_read: building a 60k-element Tcl list per
#   chunk is slow and wasteful. Issue the transaction, then let openocd move the
#   bytes natively:
#     spi_txn [concat [list 0x03] [ab $addr]] $NOR_CHUNK
#     dump_image chunk_NNN.bin [expr {$RX_BUF+4}] $NOR_CHUNK
#   and concatenate the chunks host-side. 8 MiB at NOR_CHUNK is 256 transactions.
#
# THIS UNIT
#   RDID read 0x0B 0x40 0x17 on 2026-08-01: XTX manufacturer 0x0B, model 0x4017,
#   capacity byte 0x17 = 2^23 = 8 MiB. That is XT25F64B, not GD25Q64 (0xC8 40
#   17). See src/fw/drivers/flash/bangle2_flash_ids.h. R10 12.1 names RDID as
#   the one transaction that settles this, and it did.
# =============================================================================

# --- SPIM2 registers. nRF52840 base 0x40023000. Verified against R10 12.1. ----
set SPIM_ENABLE 0x40023500; set SPIM_PSEL_SCK 0x40023508; set SPIM_PSEL_MOSI 0x4002350C
set SPIM_PSEL_MISO 0x40023510; set SPIM_FREQUENCY 0x40023524; set SPIM_RXD_PTR 0x40023534
set SPIM_RXD_MAXCNT 0x40023538; set SPIM_TXD_PTR 0x40023544; set SPIM_TXD_MAXCNT 0x40023548
set SPIM_CONFIG 0x40023554; set SPIM_ORC 0x400235C0; set SPIM_TASKS_START 0x40023010
set SPIM_EVENTS_END 0x40023118; set SPIM_RXD_AMOUNT 0x4002353C

# --- GPIO P0. Base 0x50000000. CS is a plain GPIO on P0.14, ACTIVE LOW; -------
# --- SPIM's own CSN is not used (board_bangle2.h:118). ------------------------
set P0_OUTSET 0x50000508; set P0_OUTCLR 0x5000050C
set P0_PINCNF_CS 0x50000738; set CS_BIT 0x00004000

# --- Scratch RAM. Caller MUST save and restore both regions. ------------------
# TX_BUF holds the outgoing opcode+address (4 bytes). RX_BUF receives the whole
# transaction including the 4 bytes clocked in while opcode+address went out.
set TX_BUF 0x2003F000; set RX_BUF 0x20030000

# --- Bounds used by the arithmetic-only safety guard in nor_read_max. ---------
set RAM_END 0x20040000          ;# nRF52840 has 256 KiB RAM: 0x20000000..0x2003FFFF
set RETAINED_LO 0x20000000      ;# retained page, holds the boot bits --
set RETAINED_HI 0x200000FF      ;# NEVER a scratch buffer. R10 12.1.
set SPIM_MAXCNT_LIMIT 65535     ;# RXD.MAXCNT/TXD.MAXCNT are 16-bit fields
# recommended bulk chunk: power of two, divides 8 MiB into 256 reads, and it
# sits well under the nor_read_max cap that the buffer layout actually allows
set NOR_CHUNK 32768

# --- Short-transaction detection. See "UNPROVEN CHECK" in the header. ---------
# 1 = assert RXD.AMOUNT matches RXD.MAXCNT after every transaction (default).
# Set to 0 ONLY to get past a mismatch you have decided is a quirk of the check
# rather than a short read -- and know that every backup taken with it at 0 is
# unverified against short transactions.
set SPIM_CHECK_AMOUNT 1

# Configure SPIM2 and the CS pin. Writes only to SPIM2 and GPIO P0 -- never to
# flash, never to the retained page. PSEL is only writable while the peripheral
# is disabled, so ENABLE is written last.
proc spim_setup {} {
    global SPIM_ENABLE SPIM_PSEL_SCK SPIM_PSEL_MOSI SPIM_PSEL_MISO SPIM_CONFIG
    global SPIM_FREQUENCY SPIM_ORC P0_PINCNF_CS P0_OUTSET CS_BIT
    mww $P0_PINCNF_CS 1; mww $P0_OUTSET $CS_BIT
    mww $SPIM_PSEL_SCK 16; mww $SPIM_PSEL_MOSI 15; mww $SPIM_PSEL_MISO 13
    mww $SPIM_CONFIG 0; mww $SPIM_FREQUENCY 0x80000000; mww $SPIM_ORC 0xFF; mww $SPIM_ENABLE 7
}

# One SPI transaction. txlist is the outgoing byte list; rxlen is how many extra
# bytes to clock in after it. On return RX_BUF holds [llength $txlist] junk bytes
# (clocked in while txlist went out) followed by rxlen payload bytes.
proc spi_txn {txlist rxlen} {
    global TX_BUF RX_BUF SPIM_TXD_PTR SPIM_TXD_MAXCNT SPIM_RXD_PTR SPIM_RXD_MAXCNT
    global SPIM_EVENTS_END P0_OUTCLR P0_OUTSET CS_BIT SPIM_TASKS_START
    global SPIM_RXD_AMOUNT SPIM_CHECK_AMOUNT
    set n [llength $txlist]; write_memory $TX_BUF 8 $txlist
    set want [expr {$n+$rxlen}]
    mww $SPIM_TXD_PTR $TX_BUF; mww $SPIM_TXD_MAXCNT $n
    mww $SPIM_RXD_PTR $RX_BUF; mww $SPIM_RXD_MAXCNT $want
    mww $SPIM_EVENTS_END 0; mww $P0_OUTCLR $CS_BIT; mww $SPIM_TASKS_START 1
    set done 0
    for {set i 0} {$i<500} {incr i} {
        if {[lindex [read_memory $SPIM_EVENTS_END 32 1] 0]!=0} { set done 1; break }
        sleep 2
    }
    mww $P0_OUTSET $CS_BIT
    if {$done==0} { error "SPIM timeout" }
    # UNPROVEN CHECK -- see header. EVENTS_END only says the transaction ended,
    # not that it moved every byte. RXD.AMOUNT is the byte count EasyDMA actually
    # wrote. Without this, a short transaction yields a backup that looks
    # complete and is not, which is the one failure a backup tool must never
    # have. Read AFTER EVENTS_END; the register is not valid before it.
    if {$SPIM_CHECK_AMOUNT} {
        set got [lindex [read_memory $SPIM_RXD_AMOUNT 32 1] 0]
        if {$got != $want} {
            error "SHORT TRANSACTION: RXD.AMOUNT=$got, expected $want. Data in\
                   RX_BUF is incomplete -- do NOT treat any backup built from it\
                   as valid. (If you have established this is a quirk of the\
                   check itself and not a short read, set SPIM_CHECK_AMOUNT 0.)"
        }
    }
}

# 24-bit address -> big-endian byte list, as the 0x03 command expects.
proc ab {a} { return [list [expr {($a>>16)&0xff}] [expr {($a>>8)&0xff}] [expr {$a&0xff}]] }

# Largest payload a single nor_read may request, given the current buffer
# placement. Arithmetic only -- touches no hardware. Also refuses to hand back a
# limit at all if either scratch buffer overlaps the retained page.
proc nor_read_max {} {
    global TX_BUF RX_BUF RAM_END RETAINED_LO RETAINED_HI SPIM_MAXCNT_LIMIT
    foreach {nm a sz} [list TX_BUF $TX_BUF 4 RX_BUF $RX_BUF 4] {
        if {$a <= $RETAINED_HI && ($a+$sz-1) >= $RETAINED_LO} {
            error "$nm 0x[format %08X $a] overlaps the retained page 0x20000000-0x200000FF"
        }
    }
    # 16-bit MAXCNT; must not run off the end of RAM; must not run into TX_BUF.
    set lim [expr {$SPIM_MAXCNT_LIMIT}]
    set to_ramend [expr {$RAM_END - $RX_BUF}]
    if {$to_ramend < $lim} { set lim $to_ramend }
    if {$TX_BUF > $RX_BUF} {
        set to_tx [expr {$TX_BUF - $RX_BUF}]
        if {$to_tx < $lim} { set lim $to_tx }
    }
    return [expr {$lim - 4}]   ;# 4 bytes of the RX window are opcode+address echo
}

# 0xAB, release from deep power-down. THE ONE NON-READ OPCODE IN THIS FILE.
# It is non-mutating: it erases nothing, programs nothing, and touches no data.
#
# Call it ONLY when RDID has come back all-0x00 or all-0xFF. Those two answers
# are what a part in deep power-down gives, and they are also what a genuinely
# dead bus gives -- bangle2_flash_ids.h maps both to Bangle2FlashIdDeadBus and
# cannot tell them apart. Without a wake you would read that as broken hardware
# and stop, when the fix is one transaction and 10 microseconds of patience.
#
# NEVER called implicitly. nor_read and nor_rdid will not wake the part behind
# your back; a read that fails should fail visibly, and the decision to send an
# opcode that is not a read stays with the caller.
#
# CALL IT UNCONDITIONALLY, before the first RDID. It is one transaction, it is
# non-mutating, and it is a no-op on a part that is already awake -- so there is
# nothing to gain by first guessing whether the part is asleep, and a wrong
# guess is expensive. Do NOT branch on the TEXT of an RDID: read_memory returns
# hex strings, so a sleeping part reads back as "0x0 0x0 0x0" (or "0xff 0xff
# 0xff"), never as decimal. A [string equal] against decimal text can never
# match and the guard silently never fires. Compare NUMERICALLY -- expr parses
# the 0x form -- and after an unconditional wake an all-0x00/all-0xFF RDID has
# only one meaning left: the bus is dead. Usage:
#     nor_wake                        ;# unconditional; safe on an awake part
#     set id [nor_rdid]
#     if {[lindex $id 0] == 0x00 || [lindex $id 0] == 0xFF} {
#         error "RDID [join $id] after a wake -- dead bus, not power-down"
#     }
#
# WHAT THIS AVERTS. A part left in deep power-down answers RDID with all-0x00,
# and 0x03 READ from it returns all-0x00 as well. Nothing errors and nothing is
# short: you get a backup file of exactly the right SIZE, filled with zeros,
# that looks complete by every check except opening it. Flash on the strength of
# that file and you have no copy of the system resource pack at all.
proc nor_wake {} { spi_txn [list 0xAB] 0; sleep 2 }

# RDID (0x9F). The FIRST command to issue: one transaction, and it settles
# GD25Q64 (C8 40 17) vs XT25F64B (0B 40 17) on this unit. R10 12.1.
proc nor_rdid {} {
    global RX_BUF
    spi_txn [list 0x9F] 3
    return [read_memory [expr {$RX_BUF+1}] 8 3]
}

# RDSR1 (0x05). A read opcode. Bit 0 is WIP; bits 2-6 are the block-protect
# state. Kept because it is the only way to tell that the part is quiescent and
# that the bytes a backup captures are settled data rather than a snapshot of a
# program or erase still in flight.
proc nor_status {} { global RX_BUF; spi_txn [list 0x05] 1; return [lindex [read_memory [expr {$RX_BUF+1}] 8 1] 0] }

# Single-shot preflight. A read never sets WIP, so there is nothing here to poll
# in a loop -- the write path's retry loop (nor_wait) is deliberately NOT carried
# over. What this catches is the one case that matters: a program or erase that
# was already in flight when the CPU was halted. Fail fast rather than back up
# indeterminate bytes.
proc nor_assert_idle {} {
    set s [nor_status]
    if {($s & 1) != 0} { error "WIP set (SR1=0x[format %02X $s]) -- write in flight, do not trust reads" }
    return $s
}

# READ (0x03). The only bulk data path here. Streams continuously once the
# opcode and 24-bit address are clocked out, so one transaction moves up to
# nor_read_max bytes. Returns a byte list; for bulk backup prefer dump_image
# from [RX_BUF+4] (see the header).
proc nor_read {a len} {
    global RX_BUF
    set cap [nor_read_max]
    if {$len < 1 || $len > $cap} { error "nor_read len $len out of range 1..$cap" }
    if {$a < 0 || $a > 0xFFFFFF} { error "nor_read addr 0x[format %X $a] outside the 24-bit range" }
    spi_txn [concat [list 0x03] [ab $a]] $len
    return [read_memory [expr {$RX_BUF+4}] 8 $len]
}
