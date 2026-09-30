# jtag_read_sector.tcl -- read a uPSD Flash sector over JTAG ISP.
#
# Drives the protocol directly rather than replaying an SVF, because OpenOCD's
# SVF parser stops silently on PSDsoft's "RUNTEST IDLE 8.000000E-007 SEC".
# Every value below was taken from a PSDsoft-generated SVF and verified on
# hardware:
#
#   ISC_ENABLE       IR 0x10, DR8  = 0x81, must read back 0x81
#   ISC_ADDR_SELECT  IR 0x12, DR20 = (sector << 16) | 0xFFFF
#   ISC_READ_INC     IR 0x17, DR10 prime, then DR10 per byte (8 data + 2 status)
#   ISC_DISABLE      IR 0x1E, DR8  = 0x00
#
# READ ONLY. No ISC_PGM (0x14), ISC_PGM_INC (0x15), ISC_PRE_ERASE (0x19) or
# ISC_ERASE (0x1A) appears anywhere in this file.

proc isc_enable {} {
    irscan upsd.tap 0x10 -endstate IRPAUSE
    drscan upsd.tap 8 0x81 -endstate IDLE
    runtest 1
    set rb [drscan upsd.tap 8 0x81 -endstate IDLE]
    if {![string equal -nocase $rb "81"]} {
        error "ISC_ENABLE failed: readback 0x$rb, expected 0x81"
    }
    return 1
}

proc isc_disable {} {
    irscan upsd.tap 0x1E -endstate IRPAUSE
    drscan upsd.tap 8 0x00 -endstate IDLE
    runtest 10
    drscan upsd.tap 8 0x00 -endstate IDLE
    irscan upsd.tap 0x1F -endstate IRPAUSE
    runtest 5
}

# Read $count bytes from main-Flash sector $sector, starting at its base.
proc read_sector {sector count} {
    irscan upsd.tap 0x12 -endstate IRPAUSE
    set addr [expr {($sector << 16) | 0xFFFF}]
    drscan upsd.tap 20 $addr -endstate IDLE

    irscan upsd.tap 0x17 -endstate IRPAUSE
    drscan upsd.tap 10 0x3FF -endstate IDLE
    runtest 1

    set bytes {}
    for {set i 0} {$i < $count} {incr i} {
        set v [drscan upsd.tap 10 0x3FF -endstate DRPAUSE]
        scan $v %x n
        lappend bytes [format %02X [expr {($n >> 2) & 0xFF}]]
        runtest 1
    }
    return $bytes
}
