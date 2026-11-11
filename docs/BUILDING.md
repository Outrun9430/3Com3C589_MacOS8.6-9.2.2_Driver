# Building and installing

The driver is a PowerPC CFM shared library (file type `shlb`, creator
`otan`). It has to be built with a classic Mac OS toolchain; nothing in
this repository can produce the binary on a modern machine, and **the
steps below have not been run by the author** — they are assembled from
Apple's documentation and from the `PCCardNetworkSample` project the
driver is modelled on.

## What you need

- Metrowerks CodeWarrior Pro (2.1 or later) or MPW with MrC, on a Mac or
  in an emulator (SheepShaver/QEMU are fine for *building*).
- Apple Universal Interfaces 3.2 or later.
- The Open Transport SDK (2.0.1 or later) for the kernel-module headers
  (`OpenTptModule.h`, `OpenTptLinks.h`, `OpenTptDevLinks.h`,
  `OpenTptPCISupport.h`, `dlpi.h`) and the module stub libraries.
- `dlpiether.h` and `dlpiuser.h`, the Mentat DLPI template headers. They
  are in the Open Transport SDK and also in Apple's sample:
  <https://developer.apple.com/library/archive/samplecode/PCCardNetworkSample/PCCardNetworkSample.zip>
  Copy both next to the sources in `src/`. (They are Apple/Mentat
  copyright, so they are not included here.)

The PC Card SDK is **not** needed: the driver only reads Name Registry
properties and never calls PC Card Manager.

Classic tools expect Mac line endings and MPW expects MacRoman text. If
you copy the files over from Unix, convert them first, e.g.
`tr '\n' '\r' < EL3PCCard.c > EL3PCCard.c.mac` (CodeWarrior also reads
Unix line endings directly).

## CodeWarrior

The quickest route is to start from Apple's sample project, which already
has the right libraries and settings:

1. Open `PCCardNetworkSample.mcp` and select the `myEnetDriver(PPC)`
   target.
2. Replace `EnetDrvrSrc.c` with `EL3PCCard.c`, `EnetDrvrSmpl.r` with
   `EL3PCCard.r` and `EnetDrvrSmpl.exp` with `EL3PCCard.exp`. Put
   `EL3Reg.h` and `EL3IO.h` in the same folder.
3. Point the access paths at your Open Transport SDK, as the sample's
   read-me describes.
4. Check the PPC target settings: project type *Shared Library*, file
   type `shlb`, creator `otan`, exports from the `.exp` file. The
   fragment name comes from the `cfrg` resource in `EL3PCCard.r`.
5. Build.

Starting from an empty project instead, link against the same stub
libraries that target uses: the Open Transport module libraries from the
SDK (the STREAMS kernel calls live in `OTKernelLib`, `OTEnterInterrupt`
and friends in `OTKernelUtilLib`, plus the DLPI template library that
exports the `dle_…` functions), and Apple's `DriverServicesLib` and
`NameRegistryLib` for the interrupt and Name Registry calls. No C runtime
is required.

## MPW (outline)

    MrC EL3PCCard.c -o EL3PCCard.o -opt speed -d OTKERNEL=1 ∂
        -i "{OTSDK}Open Tpt Module Interfaces:CIncludes:"
    PPCLink EL3PCCard.o <the stub libraries above> ∂
        -xm s -t shlb -c otan -@export EL3PCCard.exp ∂
        -o "3Com EtherLink III PC Card"
    Rez EL3PCCard.r -a -o "3Com EtherLink III PC Card"

## Installing

1. Put the built file in *System Folder : Extensions*.
2. Restart, insert the card, and open the TCP/IP (or AppleTalk) control
   panel. The card appears in the *Connect via* menu as an Ethernet port
   in a PC Card slot.

## If the port does not appear

The driver is located purely by name, so check the name first.

- Use a Name Registry browser (Apple's *Display Name Registry*, or the
  *PCCardDisplayNameRegistry* utility from the PC Card SDK) and find the
  card's node. Its `name` must be `pccard101,589` and its `device-type`
  `network`. If the name is different, change `kEL3ModuleName` in
  `EL3PCCard.c` and the fragment name in `EL3PCCard.r` to match (the
  fragment name is `OTModl$` + the node name) and rebuild.
- The node must have `AAPL,address` and `driver-ist` properties; they are
  created when PC Card Manager's generic enabler configures the card.
- The file must be type `shlb` and contain the `cfrg` resource.

Change `EL3_DEFAULT_PORT` in `EL3PCCard.c` to force 10BASE-T or coax
instead of automatic selection.
