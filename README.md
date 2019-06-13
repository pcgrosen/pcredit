# `pcredit`

## Overview

`pcredit` is a command line tool that allows you to read and write Private Configuration Registers (PCRs) inside the chipset of modern Intel platforms.

It has only been tested on a very small number of systems, each running a 100-series chipset. If you decide to use it yourself, be careful. I can't rule out the possibility that even trying to read some of these things could crash or even brick your system. And perhaps more obviously, the write feature of this tool can _certainly_ brick your system if it succeeds and you write to something nasty!

## Process

This process is somewhat complicated because the base address of the Private Configuration Space is hidden. It resides inside one of the base address registers of a PCI device, D31:F1 (P2SB Bridge), but this device is hidden by the BIOS. However, the hiding mechanism only blocks reads. In order to unhide it, a zero must be written to the 8th bit of offset 0xE0 (P2SBC, P2SB Control) of D31:F1; after this, the device is readable again. From there, the base address of the PCR can be read out from the first BAR of the device. (If you're trying to do this manually and would like to skip this step, on all systems on which I've performed this, the base address has been 0xfd000000. Beware, your milage may vary, and you have the potential to brick your system.)

Once the base address is known, the relevant "port number" is left shifted by 16 and added to the base address. This is the address of the port's PCR region, and a simple read/write memory operation is able to modify the registers for this port. (This tool reads/writes via `/dev/mem`.)

## Examples

Read ECTRL, which controls the state of the Direct Connect Interface (DCI) on the chipset.

```
# pcredit b8 0004
```

Disable DCI by writing a 0 to the register:

```
# pcredit b8 0004 00000000
```

## References

* The code for `pcredit` was based on the GPIO-reading code from this blog post: https://lab.whitequark.org/notes/2017-11-08/accessing-intel-ich-pch-gpios/

* The Intel 100-Series Chipset Datasheet, Volume 2, contains the relevant information for getting the SBREG_BAR and computing addresses for the port regions: https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/100-series-chipset-datasheet-vol-2.pdf
