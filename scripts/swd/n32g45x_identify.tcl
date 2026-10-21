# Read-only N32G45x identification over SWD, run by scripts/swd_identify.py.
#
# This file must only read. It never halts or resets the core and never
# touches flash, option bytes or the flash controller's key registers. On this
# family, clearing read protection mass-erases the flash, calibration included.
# tests/test_scripts.py rejects any command here that could write or reset.
#
# The adapter config (jlink or ft232h) is loaded before this file.
# Register addresses: N32G45x Series User Manual V3.1.0, sections 3.2 and 29.4.

reset_config none
gdb_port disabled
tcl_port disabled
telnet_port disabled

swd newdap n32 cpu -enable
dap create n32.dap -chain-position n32.cpu
target create n32.cpu cortex_m -dap n32.dap

init

# Prints "LPM <name> 0x........,..." or "LPM <name> error <message>".
proc lpm_read {name address words} {
    if {[catch {n32.cpu read_memory $address 32 $words} values]} {
        echo "LPM $name error [string map {"\n" " "} $values]"
        return
    }
    set out {}
    foreach word $values {
        lappend out [format 0x%08x $word]
    }
    echo "LPM $name [join $out ,]"
}

lpm_read dbg_id 0xE0042000 1
lpm_read flash_ob 0x4002201C 1
lpm_read flash_wrp 0x40022020 1
lpm_read uid 0x1FFFF7F0 3
lpm_read ucid 0x1FFFF7C0 4
lpm_read option_bytes 0x1FFFF800 5
# Main flash is unreadable over SWD at L1; an error here is itself the answer.
lpm_read boot_vectors 0x08000000 2
lpm_read app_vectors 0x0800A000 2

shutdown
