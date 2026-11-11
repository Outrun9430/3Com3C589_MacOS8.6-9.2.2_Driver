/*
 * EL3PCCard.r - resources for the 3Com EtherLink III PC Card driver.
 *
 * Open Transport finds the driver through the 'cfrg' resource: the
 * fragment name must be "OTModl$" followed by the name PC Card Manager
 * gives the card's Name Registry node ("pccard" + manufacturer ID +
 * "," + card ID from the card's CISTPL_MANFID tuple, in hex).
 */
#include "Types.r"
#include "CodeFragments.r"

resource 'cfrg' (0) {
    {
        kPowerPC,
        kFullLib,
        kNoVersionNum,
        kNoVersionNum,
        kDefaultStackSize,
        kNoAppSubFolder,
        kIsLib,
        kOnDiskFlat,
        kZeroOffset,
        kWholeFork,
        "OTModl$pccard101,589"
    };
};

resource 'vers' (1) {
    0x01, 0x00, development, 0x01,
    verUS,
    "1.0d1",
    "1.0d1, 3Com EtherLink III PC Card (3C589) driver"
};
