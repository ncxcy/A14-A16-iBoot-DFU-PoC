# PoC

PoC for iBoot dfu jump table dispatch and sep vuln map

**Tested on A14 - A16**

**Affected versions: iOS 26.0 - 26.6.0 (maybe 26.6.1)**

### iBoot vulnerability findings which i found

**finding 1: dfu payload execution path**

func 0x86bab9388 is called from fun 0x86ba707ac when ival4 == 0 and z flag == 1
it reads the dfu download buffer at 0x2006500, marks it read, then executes the payload

**finding 2: jump table dispatch (no pac)**

fun 0x86bad606c dispatches via:

    ldr w16, [x17, x16, lsl 2]   load entry from table at 0x86bb2c0a8
    add x16, x17, x16            compute absolute addr with base 0x86bb2be5c
    br  x16                      dispatch, zero pac, zero authentication

corrupting any table entry gives code exec at el3

### sep findings mapped

**vuln seprom1: 10 plain br x16 dispatch sites**

all 10 indirect branches use no pac:

    adrp  x17, table
    nop
    ldr   w16, [x17, x16, lsl 2]
    add   x16, x17, x16
    br    x16

sites: 0x0028f4 0x006384 0x006ccc 0x009710 0x00cdfc
       0x010e6c 0x014038 0x024654 0x024850 0x0249bc

**vuln seprom2: 109 unprotected return sites**

109 functions use plain ret with no pacibsp in the preceding 64 instructions.
stack corruption reachable at these sites bypasses pac entirely.

**vuln seprom3: mailbox mmio directly accessible**

sep mailbox tx: 0x5d080000
sep mailbox rx: 0x5d040000
sep aes engine: 0x5c100000

race or corruption of mailbox state during the handoff window from ap side
could allow injecting malformed sep messages

**vuln seprom4: exception vector table inline code**

vector table at 0x01e780. entries 1 through 15 contain inline arm64 code
directly in the vector slot. controlled corruption writes a gadget inline
without needing to forge a valid pointer

**vuln seprom5: memcpy loop candidates**

0x0035e4 loop cluster a (4 back branches)
0x0048b4 loop cluster b (4 back branches)

if either takes a caller controlled length with no upper bound check they
are buffer overflow candidates. needs ghidra decompile to confirm.

## requirements

macos or linux with libusb installed.

macos:

    brew install libusb

linux:

    apt install libusb-1.0-0-dev

## build

    make

## usage

device must be in dfu mode (black screen, hold side + volume down).

    sudo ./poc

optionally pass iteration count (default 5000):

    sudo ./poc 10000

the race is probabilistic. run multiple times if it does not win
typical success rate is 1 in 5 to 20 runs


if pwnd does not appear in serial, the race did not worked try running again.

## NOTE

not submitted to apple product security
no cve requested
no bug bounty claim

this is published as RESEARCH!!!!

