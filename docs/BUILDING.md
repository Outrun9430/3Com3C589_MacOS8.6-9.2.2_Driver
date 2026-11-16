# Building and installing

The driver is a PowerPC CFM shared library (file type `shlb`, creator
`otan`). It has to be built with a classic Mac OS toolchain; nothing in
this repository can produce the binary on a modern machine, and **the
steps below have not been run by the author** — they are assembled from
Apple's documentation and from the `PCCardNetworkSample` project the
driver is modelled on.

## What you need

Everything except the compiler is on Apple's *Mac OS SDK* developer CDs
(the October 1998 edition is known to have all of it, under
*Development Kits*):

- Metrowerks CodeWarrior Pro, or MPW with MrC.
- `Interfaces&Libraries` from the SDK CD (Apple's Universal Interfaces
  of the same vintage as the Open Transport SDK). Use these rather than
  a newer set: Universal Interfaces 3.3 and later renamed the Open
  Transport kernel headers.
- `Open Transport 1.3 : Open Transport SDK`, both the
  `Open Tpt Module Developer` and `Open Tpt Client Developer` folders.
- Optional: `PC Card SDKs : PC Card Manager 3.0` for the `DispNameReg`
  and `TupleDumper` utilities. The driver itself never calls PC Card
  Manager and does not need this SDK to build.

From `Open Tpt Module Developer` you use:

| Item | Purpose |
|---|---|
| `Includes` | `OpenTptModule.h`, `OpenTptDevLinks.h`, `OpenTptPCISupport.h`, `dlpi.h`, ... |
| `Libraries : OpenTptModuleLib` | PowerPC stub library: STREAMS kernel calls |
| `Libraries : OpenTptKernelUtilLib` | PowerPC stub library: `OTEnterInterrupt`, `OTAllocMem`, ... |
| `Samples : DLPI Template : dlpiether.c` | The DLPI template itself. It is **source that is compiled into the driver**, not a library. |
| `Samples : DLPI Template : dlpiether.h`, `dlpiuser.h` | Its headers. Use these, not the copies in Apple's PCCardNetworkSample, so they match `dlpiether.c`. |
| `Documents : DLPI Template.pdf` | The template's documentation |

The `.o` files in `Libraries` are for 68K and are not used.

Copy `dlpiether.c`, `dlpiether.h` and `dlpiuser.h` next to the sources
in `src/` (they are Apple/Mentat copyright, so they are not in this
repository).

Classic tools expect Mac line endings. If you copy the sources over from
Unix, CodeWarrior reads them as they are; for MPW convert them first.

## CodeWarrior

Create a new, empty Mac OS PowerPC project and add:

- `EL3PCCard.c`, `dlpiether.c`, `EL3PCCard.r`
- `OpenTptModuleLib`, `OpenTptKernelUtilLib` (from the SDK)
- `DriverServicesLib`, `NameRegistryLib` (stub libraries from
  `Interfaces&Libraries`)

Do not add MSL or `InterfaceLib`; if the linker then reports an
unresolved C library routine from `dlpiether.c`, add only the library
that supplies it.

Target settings:

- *Access Paths*: `src/`, the SDK's module and client `Includes`
  folders, and the `CIncludes` folder of `Interfaces&Libraries`. Remove
  (or order last) CodeWarrior's own *MacOS Support* headers so the
  older, matching set is the one found.
- *PPC Target*: project type *Shared Library*, creator `otan`, type
  `shlb`.
- *PPC Linker*: clear the *Main* entry point field.
- *PPC PEF*: export symbols *Use ".exp" file* (`EL3PCCard.exp`, named
  to match the project).
- *C/C++ Language*: a prefix file containing `#define OTKERNEL 1`, so
  that `dlpiether.c` is compiled as kernel code as well.

The fragment name comes from the `cfrg` resource in `EL3PCCard.r`.

## MPW (outline)

    MrC EL3PCCard.c -o EL3PCCard.o -opt speed -d OTKERNEL=1 -i <includes>
    MrC dlpiether.c -o dlpiether.o -opt speed -d OTKERNEL=1 -i <includes>
    PPCLink EL3PCCard.o dlpiether.o OpenTptModuleLib OpenTptKernelUtilLib
        DriverServicesLib NameRegistryLib
        -xm s -t shlb -c otan -@export EL3PCCard.exp
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
  *DispNameReg* utility from the PC Card SDK) and find the
  card's node. Its `name` must be `pccard101,589` and its `device-type`
  `network`. If the name is different, change `kEL3ModuleName` in
  `EL3PCCard.c` and the fragment name in `EL3PCCard.r` to match (the
  fragment name is `OTModl$` + the node name) and rebuild.
- The node must have `AAPL,address` and `driver-ist` properties; they are
  created when PC Card Manager's generic enabler configures the card.
- The file must be type `shlb` and contain the `cfrg` resource.

Change `EL3_DEFAULT_PORT` in `EL3PCCard.c` to force 10BASE-T or coax
instead of automatic selection.
