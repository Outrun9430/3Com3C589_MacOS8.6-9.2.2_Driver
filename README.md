# Ethernet-OS9

An Open Transport Ethernet driver for the **3Com EtherLink III LAN PC Card
(3C589 / 3C589B / 3C589C / 3C589D)** on PowerPC PowerBooks running
**Mac OS 8.6 – 9.2.2**.

> **Status: untested on real hardware.** The driver has never been
> compiled with a Mac compiler or run on a PowerBook. Its logic has been
> exercised on a development host against a register-level model of the
> card (`make -C test`), which passes. Expect to do some bring-up work;
> see [docs/BUILDING.md](docs/BUILDING.md).

## Why the 3C589D

Three cards were candidates:

| Card | Interface | Chip | Verdict |
|---|---|---|---|
| 3Com EtherLink III **3C589D** | 16-bit PC Card, 5 V, 10BASE-T + coax | 3Com EtherLink III (3C509 family) | **Chosen** |
| 3Com Fast EtherLink **3C574-TX** | 16-bit PC Card, 10/100 | 3Com "Vortex"-style with MII PHY | Good second target |
| Linksys EtherFast 10/100 Integrated | 16-bit *or* CardBus depending on model | NE2000 clone (DL10019 / AX88190) or a CardBus chip | Not chosen |

- **One well-documented chip.** Every 3C589 revision uses the same
  EtherLink III register set, with long-lived open-source drivers to
  cross-check against. The card was also sold rebadged for PowerBooks
  (Farallon EtherMac PC Card), so the hardware is known to work in Mac
  PC Card sockets.
- **Works in every eligible PowerBook.** It is a plain 16-bit, 5 V,
  I/O-mapped card with one interrupt — the kind of card PC Card Manager
  3.0's generic enabler configures by itself, so no custom enabler is
  needed.
- **Self-contained.** The MAC address is in the card's EEPROM and there
  is no PHY to manage, so the driver needs no PC Card Manager calls at
  all.
- The **3C574-TX** shares most of the command set but adds an MII
  transceiver and different configuration registers; it would be a
  separate driver built on this one. 100 Mbit is of limited benefit
  through a 16-bit PC Card socket.
- The **Linksys** label covers several different chipsets across
  hardware revisions, some needing per-chip quirks, and I can't tell
  from the photo which one this is (or whether it is 16-bit or CardBus).

## Supported systems

PowerBooks that use **PC Card Manager 3.0**: PowerBook 2400, 3400, G3,
G3 Series (Wallstreet, Lombard, Pismo) and the PowerBook G4, on Mac OS
8.6 through 9.2.2 with Open Transport.

Not supported: the PowerBook 190/5300/1400, which use the older PC Card
Manager 2.0 and need a different (68K-style) driver.

## How it works

- PC Card Manager names the card's Name Registry node after its
  manufacturer/card ID — `pccard101,589` — and, because the card
  identifies itself as a network card, registers an Ethernet port with
  Open Transport under that name.
- Open Transport loads the CFM fragment `OTModl$pccard101,589` (this
  driver) from the Extensions folder and calls `InitStreamModule`.
- DLPI protocol handling is done by Apple's/Mentat's DLPI template
  (`dlpiether`); this driver supplies the hardware half.
- The interrupt handler only masks the card and wakes the driver's
  STREAMS service routine, which does all chip I/O. A one-second
  watchdog recovers lost interrupts and reprograms the chip after the
  PowerBook sleeps (the socket is powered off during sleep).
- 10BASE-T is used by default, falling back to coax when there is no
  link beat.

Design notes are in the header comment of
[src/EL3PCCard.c](src/EL3PCCard.c).

## Layout

| Path | Contents |
|---|---|
| `src/EL3PCCard.c` | The driver |
| `src/EL3Reg.h` | EtherLink III register definitions |
| `src/EL3IO.h` | Memory-mapped I/O access (byte order, `eieio`) |
| `src/EL3PCCard.r`, `.exp` | `cfrg`/`vers` resources and export list |
| `test/` | Host-side chip model, mock Open Transport, and tests |
| `docs/BUILDING.md` | Building, installing, troubleshooting |

## Testing on a development host

    make -C test

This compiles the driver against stub headers and runs it against a
model of the 3C589 that checks register-window discipline, FIFO framing,
padding, back-pressure, error recovery, media selection, sleep/wake
recovery and hot removal. It verifies the driver against *my reading* of
the chip and of Open Transport, not against the real things.

## Known unknowns

These are the assumptions most worth checking first on a real machine:

1. The device node is named `pccard101,589` and has `device-type`
   `network` (the 3C589's CIS should produce both).
2. The stream byte order of 16-bit FIFO accesses through the PowerBook's
   PC Card bridge (`el3_fifo_read`/`el3_fifo_write` in `EL3IO.h`). If
   received data arrives with each byte pair swapped, swap there.
3. Exact prototypes of a few Open Transport kernel calls
   (`mi_timer_alloc`, `mi_timer`) differ between SDK versions; the
   compiler will say so.
4. The DLPI template's stub library name for linking (see
   `docs/BUILDING.md`).

## References

- Apple, *PCCardNetworkSample* (PC Card Ethernet driver sample for PC
  Card Manager 3.0) —
  <https://developer.apple.com/library/archive/samplecode/PCCardNetworkSample/Introduction/Intro.html>
- Apple, *Designing PCI Cards and Drivers for Power Macintosh Computers*,
  ch. 14 "Network Drivers" —
  <https://developer.apple.com/library/archive/documentation/Hardware/DeviceManagers/pci_srvcs/pci_cards_drivers/Designing_PCI_Cards_Drivers.pdf>
- Apple Technote 1099, *Power Management & PC Card Manager 3.0* —
  <https://developer.apple.com/library/archive/technotes/tn/tn1099.html>
- Linux `3c589_cs.c` and `3c574_cs.c` (register behaviour of the PC Card
  variants; no code was copied).

The driver's structure descends from the hardware-driver skeleton in
Apple's sample code (itself based on Mentat, SystemSoft and Ratoc code)
and has been substantially changed; it is not Apple sample code.
