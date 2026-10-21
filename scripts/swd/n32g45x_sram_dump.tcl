# Read-only SRAM snapshot of a running N32G45x over SWD, run by scripts/swd_sram_dump.py.
#
# Same rules as n32g45x_identify.tcl: this file must only read. The core keeps
# running (memory is read through the AHB access port), so the snapshot is not
# atomic. tests/test_scripts.py rejects any command here that could write or reset.

reset_config none
gdb_port disabled
tcl_port disabled
telnet_port disabled

swd newdap n32 cpu -enable
dap create n32.dap -chain-position n32.cpu
target create n32.cpu cortex_m -dap n32.dap

init

# N32G455: 144 KB SRAM from 0x20000000 (DBG_ID read 2026-10-19).
set lpm_base 0x20000000
set lpm_bytes 0x24000
set lpm_chunk_words 1024

# Prints "LPM sram <address> <word>,<word>,..." or "LPM sram <address> error <message>".
proc lpm_chunk {address words} {
    if {[catch {n32.cpu read_memory $address 32 $words} values]} {
        echo "LPM sram [format 0x%08x $address] error [string map {"\n" " "} $values]"
        return 0
    }
    set out {}
    foreach word $values {
        lappend out [format %08x $word]
    }
    echo "LPM sram [format 0x%08x $address] [join $out ,]"
    return 1
}

# Probe one word first, so a blocked SRAM costs one read instead of 36.
if {[lpm_chunk $lpm_base 1]} {
    for {set offset 0} {$offset < $lpm_bytes} {incr offset [expr {$lpm_chunk_words * 4}]} {
        lpm_chunk [expr {$lpm_base + $offset}] $lpm_chunk_words
    }
}

shutdown
