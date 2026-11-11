/*
 * EL3PCCard.c - Open Transport DLPI driver for the 3Com EtherLink III
 *               LAN PC Card (3C589 / 3C589B / 3C589C / 3C589D).
 *
 * Target: Mac OS 8.6 - 9.2.2 on PowerPC PowerBooks with PC Card Manager 3.0
 * (PowerBook 2400, 3400, G3, G3 Series, G4).
 *
 * How the pieces fit together
 * ---------------------------
 * PC Card Manager 3.0 creates a Name Registry node for the inserted card,
 * named after its CISTPL_MANFID tuple ("pccard101,589" for the 3C589), and
 * its generic enabler configures the card and publishes the mapped I/O
 * window ("AAPL,address") and interrupt ("driver-ist").  Because the card's
 * function ID is "network", the system's PC Card port scanner registers an
 * Ethernet port with Open Transport whose module name is the node name.
 * Open Transport then loads the CFM fragment "OTModl$pccard101,589" - this
 * file - and calls InitStreamModule() with the node's RegEntryID.
 *
 * The DLPI protocol work (binds, SAP demultiplexing, multicast lists,
 * fast path, statistics requests ...) is done by Apple's/Mentat's DLPI
 * template ("dlpiether"); this file supplies the hardware half.
 *
 * Concurrency model
 * -----------------
 * The module is installed with SQLVL_MODULE, so Open Transport never runs
 * two of its STREAMS entry points at once.  ALL chip programming happens
 * in those entry points.  The primary interrupt handler does the minimum:
 * it masks the card's interrupt, acknowledges the latch and queues a small
 * "kick" message on a read queue, which makes Open Transport run the read
 * service routine; that routine drains the receiver and handles every
 * other event, then unmasks the card.  The interrupt handler only ever
 * touches the command/status register (valid in every register window),
 * so it cannot disturb a register sequence it happens to interrupt.
 *
 * A one second watchdog (an Open Transport timer message) recovers from
 * anything that can be lost: a kick that could not be allocated, a missed
 * interrupt, and - importantly on a PowerBook - the chip losing all of its
 * state when the socket is powered down for sleep.
 *
 * Structure derived from the hardware-driver skeleton in Apple's
 * "PCCardNetworkSample" sample code (itself descended from Mentat's DLPI
 * template loopback driver and SystemSoft/Ratoc sample code); it has been
 * substantially changed and is not Apple sample code.
 */

#ifndef OTKERNEL
#define OTKERNEL 1
#endif

#ifdef EL3_HOST_TEST
#include "host_stubs.h"
#else
#include <OpenTptModule.h>
#include <OpenTptLinks.h>
#include <OpenTptDevLinks.h>
#include <OpenTptPCISupport.h>
#include <dlpi.h>
#include <NameRegistry.h>
#include <DriverServices.h>
#include <Interrupts.h>
#include "dlpiuser.h"
#include "dlpiether.h"
#endif

#include "EL3Reg.h"
#include "EL3IO.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                      */
/* ------------------------------------------------------------------ */

/*
 * Must equal the name PC Card Manager gives the card's device node, and
 * the fragment name in the 'cfrg' resource must be "OTModl$" + this.
 */
#ifndef kEL3ModuleName
#define kEL3ModuleName      "pccard101,589"
#endif

/* Transceiver selection. */
#define EL3_PORT_AUTO       0       /* 10BASE-T, fall back to coax      */
#define EL3_PORT_TP         1
#define EL3_PORT_COAX       2
#define EL3_PORT_AUI        3

#ifndef EL3_DEFAULT_PORT
#define EL3_DEFAULT_PORT    EL3_PORT_AUTO
#endif

#define EL3_MAX_FRAME       1514    /* without FCS                      */
#define EL3_MIN_FRAME       60
#define EL3_TXBUF_BYTES     1516    /* EL3_MAX_FRAME rounded to 4       */
#define EL3_TX_ROOM         1536    /* TxAvailable threshold            */
#define EL3_HIWAT           8192
#define EL3_LOWAT           1024
#define EL3_TICK_MS         1000
#define EL3_RX_BURST        32      /* frames per service pass          */
#define EL3_SERVICE_PASSES  8
#define EL3_LINK_MISSES     2       /* ticks without link beat -> coax  */

#define EL3_INT_BASE        (ST_IntLatch | ST_AdapterFailure | ST_TxComplete | \
                             ST_RxComplete | ST_StatsFull)

/* ------------------------------------------------------------------ */
/* Data structures                                                    */
/* ------------------------------------------------------------------ */

/* Per-stream data (q_ptr); the template's dcl_t must come first. */
typedef struct el3_stream_s {
    dcl_t                   dcl;
    struct el3_stream_s     *next;
    queue_t                 *rq;
} el3_stream_t;

/* Per-card data; the template's dle_t must come first. */
typedef struct el3_board_s {
    dle_t                   dle;

    void                    *io;            /* mapped I/O base          */

    InterruptSetMember      ist;
    void                    *oldRefCon;
    InterruptHandler        oldHandler;
    InterruptEnabler        intEnabler;
    InterruptDisabler       intDisabler;
    UInt8                   isrInstalled;

    UInt8                   running;        /* between hw_start/hw_stop */
    UInt8                   inService;
    UInt8                   txWaiting;      /* TxAvailable armed        */
    UInt8                   cardGone;       /* reads come back all ones */
    UInt8                   needWake;       /* restart blocked writers  */
    UInt8                   ifPort;         /* EL3_PORT_xxx requested   */
    UInt8                   curPort;        /* EL3_PORT_xxx in use      */
    UInt8                   linkMisses;
    UInt16                  rxFilter;

    /* Shared with the primary interrupt handler. */
    queue_t * volatile      svcQ;           /* read queue to kick       */
    volatile UInt32         isrMasked;      /* card masked, kick queued */
    volatile UInt32         workPending;    /* kick could not be queued */

    el3_stream_t            *streams;
    mblk_t                  *timerMp;
    el3_stream_t            *timerOwner;

    UInt32                  tickCarrierErrs;
    UInt32                  txBuf[EL3_TXBUF_BYTES / 4];
} el3_board_t;

static el3_board_t  *el3_global;

/* ------------------------------------------------------------------ */
/* Prototypes                                                         */
/* ------------------------------------------------------------------ */

install_info    *GetOTInstallInfo(void);
Boolean         InitStreamModule(void *systemDependent);
void            TerminateStreamModule(void);
OTResult        ValidateHardware(RegEntryID *our_id);

static int      el3_open(queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp);
static int      el3_close(queue_t *q, int flag, cred_t *credp);
static int      el3_wput(queue_t *q, mblk_t *mp);
static int      el3_wsrv(queue_t *q);
static int      el3_rsrv(queue_t *q);

static void     el3_hw_start(void *boardvp);
static void     el3_hw_stop(void *boardvp);
static void     el3_hw_address_filter_reset(void *boardvp, dle_addr_t *addr_list,
                    UInt32 addr_count, UInt32 promisc_count,
                    UInt32 multi_promisc_count, UInt32 accept_broadcast,
                    UInt32 accept_errors);

static void     el3_service(el3_board_t *bp);

/* ------------------------------------------------------------------ */
/* STREAMS configuration                                              */
/* ------------------------------------------------------------------ */

static struct module_info el3_minfo = {
    kEnetModuleID, kEL3ModuleName, 0, EL3_MAX_FRAME, EL3_HIWAT, EL3_LOWAT
};

/* Drivers have no read put procedure. */
static struct qinit el3_rinit = {
    NULL, el3_rsrv, el3_open, el3_close, NULL, &el3_minfo, NULL
};

static struct qinit el3_winit = {
    el3_wput, el3_wsrv, el3_open, el3_close, NULL, &el3_minfo, NULL
};

static struct streamtab el3_streamtab = { &el3_rinit, &el3_winit, NULL, NULL };

static install_info el3_install_info = {
    &el3_streamtab,
    kOTModIsDriver | kOTModIsPortDriver | kOTModUpperIsDLPI | kOTModUsesInterrupts,
    SQLVL_MODULE,
    NULL,
    0,
    0
};

/* ------------------------------------------------------------------ */
/* Small chip helpers (STREAMS context only unless noted)              */
/* ------------------------------------------------------------------ */

#define EL3_CMD_WR(bp, c)   el3_wr16((bp)->io, EL3_CMD, (UInt16)(c))
#define EL3_WINDOW(bp, n)   EL3_CMD_WR(bp, CMD_SelectWindow | (n))

/* Issue a command that takes time and wait (bounded) for it to finish. */
static void el3_cmd_wait(el3_board_t *bp, UInt16 cmd)
{
    int i;

    EL3_CMD_WR(bp, cmd);
    for (i = 0; i < 2000; i++) {
        if (!(el3_rd16(bp->io, EL3_STATUS) & ST_CmdBusy))
            break;
    }
}

/* Window 0 must be selected. */
static UInt16 el3_read_eeprom(el3_board_t *bp, int index)
{
    int i;

    el3_wr16(bp->io, W0_EEPROM_CMD, (UInt16)(EEPROM_READ + index));
    for (i = 0; i < 4000; i++) {
        if (!(el3_rd16(bp->io, W0_EEPROM_CMD) & EEPROM_BUSY))
            break;
    }
    return el3_rd16(bp->io, W0_EEPROM_DATA);
}

/* Program the interrupt mask unless the interrupt path currently owns it. */
static void el3_set_int_mask(el3_board_t *bp)
{
    UInt16 mask;

    if (!bp->running || bp->inService || bp->isrMasked || bp->cardGone)
        return;
    mask = EL3_INT_BASE;
    if (bp->txWaiting)
        mask |= ST_TxAvailable;
    EL3_CMD_WR(bp, CMD_SetIntrEnb | mask);
}

static void el3_set_xcvr(el3_board_t *bp, UInt8 port)
{
    UInt16 sel;

    switch (port) {
    case EL3_PORT_COAX: sel = XCVR_COAX;    break;
    case EL3_PORT_AUI:  sel = XCVR_AUI;     break;
    default:            sel = XCVR_TP;      port = EL3_PORT_TP; break;
    }
    EL3_WINDOW(bp, 0);
    el3_wr16(bp->io, W0_ADDR_CONFIG, sel);
    EL3_CMD_WR(bp, port == EL3_PORT_COAX ? CMD_StartCoax : CMD_StopCoax);
    EL3_WINDOW(bp, 4);
    el3_wr16(bp->io, W4_MEDIA,
             (UInt16)(MEDIA_LED | (port == EL3_PORT_TP ? MEDIA_TP : 0)));
    EL3_WINDOW(bp, 1);
    bp->curPort = port;
    bp->linkMisses = 0;
}

/* Read (and thereby clear) the chip's statistics counters. */
static void el3_update_stats(el3_board_t *bp)
{
    dle_ethernet_status_t *es = &bp->dle.dle_estatus;
    UInt32 n;

    EL3_CMD_WR(bp, CMD_StatsDisable);
    EL3_WINDOW(bp, 6);
    n = el3_rd8(bp->io, W6_CARRIER_LOST);
    es->transmit_carrier_sense_errors += n;
    bp->tickCarrierErrs += n;
    es->transmit_sqe_test_errors            += el3_rd8(bp->io, W6_SQE_ERRORS);
    es->transmit_multiple_collision_frames  += el3_rd8(bp->io, W6_MULTI_COLL);
    es->transmit_single_collision_frames    += el3_rd8(bp->io, W6_SINGLE_COLL);
    es->transmit_late_collisions            += el3_rd8(bp->io, W6_LATE_COLL);
    es->receive_internal_mac_errors         += el3_rd8(bp->io, W6_RX_OVERRUN);
    (void)el3_rd8(bp->io, W6_TX_FRAMES_OK);
    (void)el3_rd8(bp->io, W6_RX_FRAMES_OK);
    es->transmit_deferred_transmissions     += el3_rd8(bp->io, W6_TX_DEFERRALS);
    (void)el3_rd16(bp->io, W6_RX_BYTES_OK);
    (void)el3_rd16(bp->io, W6_TX_BYTES_OK);
    EL3_WINDOW(bp, 1);
    EL3_CMD_WR(bp, CMD_StatsEnable);
}

/* Bring the chip from any state to fully operational. */
static void el3_hw_init(el3_board_t *bp)
{
    int i;

    bp->cardGone = 0;
    bp->txWaiting = 0;
    bp->isrMasked = 0;
    bp->workPending = 0;

    EL3_CMD_WR(bp, CMD_SetIntrEnb | 0);
    el3_cmd_wait(bp, CMD_RxReset);
    el3_cmd_wait(bp, CMD_TxReset);

    EL3_WINDOW(bp, 0);
    el3_wr16(bp->io, W0_CONFIG_CTRL, 0x0001);       /* activate adapter */
    el3_wr16(bp->io, W0_RESOURCE_CONFIG, 0x3F00);   /* required on PC Card */

    EL3_WINDOW(bp, 2);
    for (i = 0; i < 6; i++)
        el3_wr8(bp->io, i, bp->dle.dle_current_addr[i]);

    if (bp->ifPort == EL3_PORT_AUTO)
        el3_set_xcvr(bp, bp->curPort == EL3_PORT_COAX ? EL3_PORT_COAX : EL3_PORT_TP);
    else
        el3_set_xcvr(bp, bp->ifPort);

    /* Clear the statistics by reading them (discard what we read). */
    EL3_CMD_WR(bp, CMD_StatsDisable);
    EL3_WINDOW(bp, 6);
    for (i = 0; i < 9; i++)
        (void)el3_rd8(bp->io, i);
    (void)el3_rd16(bp->io, W6_RX_BYTES_OK);
    (void)el3_rd16(bp->io, W6_TX_BYTES_OK);

    EL3_WINDOW(bp, 1);
    EL3_CMD_WR(bp, CMD_SetRxFilter | bp->rxFilter);
    EL3_CMD_WR(bp, CMD_StatsEnable);
    EL3_CMD_WR(bp, CMD_RxEnable);
    EL3_CMD_WR(bp, CMD_TxEnable);
    EL3_CMD_WR(bp, CMD_SetStatusEnb | 0xFF);
    EL3_CMD_WR(bp, CMD_AckIntr | ST_IntLatch | ST_TxAvailable | ST_RxEarly | ST_IntReq);
    bp->needWake = 1;       /* anything queued for transmit can go now */
    el3_set_int_mask(bp);
}

/* Restart the write service routines of streams with queued frames. */
static void el3_wake_writers(el3_board_t *bp)
{
    el3_stream_t *sp;
    queue_t *wq;

    bp->needWake = 0;
    for (sp = bp->streams; sp != NULL; sp = sp->next) {
        wq = WR(sp->rq);
        if (wq->q_first != NULL)
            qenable(wq);
    }
}

/* Empty the transmit status stack, restarting the transmitter on error. */
static void el3_pop_tx_status(el3_board_t *bp)
{
    dle_ethernet_status_t *es = &bp->dle.dle_estatus;
    UInt8 st;
    int i;

    for (i = 0; i < 32; i++) {
        st = el3_rd8(bp->io, W1_TX_STATUS);
        if (st == 0xFF || !(st & (TXS_Complete | TXS_StatusOverflow)))
            break;
        if (st & (TXS_Jabber | TXS_Underrun)) {
            /* The FIFO contents are unusable; this also empties it. */
            el3_cmd_wait(bp, CMD_TxReset);
            es->transmit_internal_mac_errors++;
            bp->txWaiting = 0;
            bp->needWake = 1;
        }
        if (st & TXS_MaxCollisions)
            es->transmit_excessive_collisions++;
        if (st & (TXS_Jabber | TXS_Underrun | TXS_MaxCollisions)) {
            bp->dle.dle_istatus.transmit_errors++;
            EL3_CMD_WR(bp, CMD_TxEnable);
        }
        el3_wr8(bp->io, W1_TX_STATUS, 0);       /* pop */
    }
}

/* Drain completed frames from the receive FIFO and pass them upstream. */
static void el3_rx(el3_board_t *bp)
{
    dle_ethernet_status_t *es = &bp->dle.dle_estatus;
    UInt16 rxs;
    UInt32 len;
    mblk_t *mp;
    int limit = EL3_RX_BURST;

    while (limit-- > 0) {
        rxs = el3_rd16(bp->io, W1_RX_STATUS);
        if (rxs & RXS_Incomplete)
            break;                      /* nothing (more) to read       */
        len = rxs & RXS_LenMask;
        if (rxs & RXS_Error) {
            switch (rxs & RXS_ErrMask) {
            case 0x0000:    es->receive_internal_mac_errors++;  break;  /* overrun   */
            case 0x0800:    es->receive_frames_too_long++;      break;  /* oversize  */
            case 0x2000:    es->receive_alignment_errors++;     break;  /* alignment */
            case 0x2800:    es->receive_fcs_errors++;           break;  /* CRC       */
            default:        break;                                      /* runt, dribble */
            }
            bp->dle.dle_istatus.receive_errors++;
        } else if (len < 14 || len > EL3_MAX_FRAME + 4) {
            bp->dle.dle_istatus.receive_errors++;
        } else if ((mp = allocb(len + 4, BPRI_LO)) == NULL) {
            bp->dle.dle_istatus.receive_discards++;
        } else {
            /* Offset by two so the IP header lands on a 4-byte boundary. */
            mp->b_rptr = mp->b_datap->db_base + 2;
            el3_fifo_read(bp->io, mp->b_rptr, (len + 1) >> 1);
            mp->b_wptr = mp->b_rptr + len;
            dle_inbound(&bp->dle, mp);
        }
        el3_cmd_wait(bp, CMD_RxDiscard);
    }
}

static void el3_adapter_failure(el3_board_t *bp)
{
    UInt16 diag;

    EL3_WINDOW(bp, 4);
    diag = el3_rd16(bp->io, W4_FIFO_DIAG);
    EL3_WINDOW(bp, 1);
    if (diag & FIFOD_TxOverrun) {
        el3_cmd_wait(bp, CMD_TxReset);
        EL3_CMD_WR(bp, CMD_TxEnable);
        bp->dle.dle_estatus.transmit_internal_mac_errors++;
        bp->txWaiting = 0;
        bp->needWake = 1;
    }
    if (diag & FIFOD_RxUnderrun) {
        el3_cmd_wait(bp, CMD_RxReset);
        EL3_CMD_WR(bp, CMD_SetRxFilter | bp->rxFilter);
        EL3_CMD_WR(bp, CMD_RxEnable);
        bp->dle.dle_estatus.receive_internal_mac_errors++;
    }
    EL3_CMD_WR(bp, CMD_AckIntr | ST_AdapterFailure);
}

/*
 * Handle everything the chip is reporting, then unmask its interrupt.
 * Runs in STREAMS context (read service, watchdog, close).
 */
static void el3_service(el3_board_t *bp)
{
    UInt16 status, work;
    int pass, again;

    if (!bp->running || bp->inService)
        return;
    bp->inService = 1;

    for (again = 0; again < 3; again++) {
        bp->workPending = 0;
        EL3_CMD_WR(bp, CMD_SetIntrEnb | 0);

        for (pass = 0; pass < EL3_SERVICE_PASSES; pass++) {
            status = el3_rd16(bp->io, EL3_STATUS);
            if (status == 0xFFFF) {
                bp->cardGone = 1;       /* ejected, or socket powered off */
                break;
            }
            if ((status & ST_WindowMask) != ST_Window1)
                break;                  /* chip was reset; watchdog re-inits */
            work = status & (ST_RxComplete | ST_TxComplete | ST_StatsFull |
                             ST_AdapterFailure | ST_RxEarly);
            if (bp->txWaiting)
                work |= status & ST_TxAvailable;
            if (!work)
                break;

            if (work & ST_RxComplete)
                el3_rx(bp);
            if (work & ST_TxAvailable) {
                EL3_CMD_WR(bp, CMD_AckIntr | ST_TxAvailable);
                bp->txWaiting = 0;
                bp->needWake = 1;
            }
            if (work & ST_TxComplete)
                el3_pop_tx_status(bp);
            if (work & ST_StatsFull)
                el3_update_stats(bp);
            if (work & ST_RxEarly)
                EL3_CMD_WR(bp, CMD_AckIntr | ST_RxEarly);
            if (work & ST_AdapterFailure)
                el3_adapter_failure(bp);
        }

        if (bp->cardGone)
            break;

        EL3_CMD_WR(bp, CMD_AckIntr | ST_IntReq | ST_IntLatch);
        bp->isrMasked = 0;
        bp->inService = 0;
        el3_set_int_mask(bp);
        bp->inService = 1;

        /*
         * If an event slipped in before the unmask and did not raise the
         * interrupt latch, nobody would come back for it: go round again.
         */
        status = el3_rd16(bp->io, EL3_STATUS);
        if (status == 0xFFFF || (status & ST_IntLatch) || bp->isrMasked)
            break;
        if (!(status & (ST_RxComplete | ST_TxComplete | ST_StatsFull | ST_AdapterFailure)))
            break;
    }

    bp->inService = 0;
    if (bp->needWake)
        el3_wake_writers(bp);
}

/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */

/*
 * Try to hand one frame to the chip.  Returns 1 if the message was
 * consumed (sent or dropped), 0 if the FIFO has no room; in that case the
 * message is untouched and a TxAvailable interrupt has been requested.
 */
static int el3_xmit(el3_board_t *bp, mblk_t *first_mp)
{
    dle_interface_status_t *is = &bp->dle.dle_istatus;
    UInt8 *buf = (UInt8 *)bp->txBuf;
    mblk_t *mp;
    UInt32 total = 0, wire, n;
    UInt16 status;

    if (!bp->running || bp->cardGone) {
        is->transmit_discards++;
        freemsg(first_mp);
        return 1;
    }

    /* Gather the message into the word-aligned bounce buffer. */
    for (mp = first_mp; mp != NULL; mp = mp->b_cont) {
        if (mp->b_wptr <= mp->b_rptr)
            continue;
        n = (UInt32)(mp->b_wptr - mp->b_rptr);
        if (total + n > EL3_MAX_FRAME) {
            is->transmit_errors++;
            freemsg(first_mp);
            return 1;
        }
        bcopy((char *)mp->b_rptr, (char *)buf + total, n);
        total += n;
    }
    if (total < 14) {
        is->transmit_errors++;
        freemsg(first_mp);
        return 1;
    }

    /* The template leaves the 802.3 length field zero for us to fill in. */
    if (!(buf[12] | buf[13])) {
        buf[12] = (UInt8)((total - 14) >> 8);
        buf[13] = (UInt8)(total - 14);
    }

    /* Pad short frames, then round up to the FIFO's 4-byte granularity. */
    wire = total;
    while (wire < EL3_MIN_FRAME)
        buf[wire++] = 0;
    for (n = wire; n & 3; n++)
        buf[n] = 0;

    status = el3_rd16(bp->io, EL3_STATUS);
    if (status == 0xFFFF) {
        bp->cardGone = 1;
        is->transmit_discards++;
        freemsg(first_mp);
        return 1;
    }
    if (el3_rd16(bp->io, W1_TX_FREE) < n + 4) {
        EL3_CMD_WR(bp, CMD_SetTxThreshold | EL3_TX_ROOM);
        bp->txWaiting = 1;
        el3_set_int_mask(bp);
        return 0;
    }

    /* Four byte preamble (length, reserved), then the data. */
    el3_wr16(bp->io, W1_TX_FIFO, (UInt16)wire);
    el3_wr16(bp->io, W1_TX_FIFO, 0);
    el3_fifo_write(bp->io, buf, n >> 1);

    is->bytes_sent += wire;
    if (buf[0] & 1) {
        if ((buf[0] & buf[1] & buf[2] & buf[3] & buf[4] & buf[5]) == 0xFF)
            is->broadcast_frames_sent++;
        else
            is->multicast_frames_sent++;
    } else
        is->unicast_frames_sent++;

    el3_pop_tx_status(bp);
    freemsg(first_mp);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Watchdog                                                           */
/* ------------------------------------------------------------------ */

static void el3_media_check(el3_board_t *bp)
{
    UInt16 media;

    if (bp->ifPort != EL3_PORT_AUTO)
        return;
    if (bp->curPort == EL3_PORT_TP) {
        EL3_WINDOW(bp, 4);
        media = el3_rd16(bp->io, W4_MEDIA);
        EL3_WINDOW(bp, 1);
        if (media & MEDIA_LinkBeat)
            bp->linkMisses = 0;
        else if (++bp->linkMisses >= EL3_LINK_MISSES)
            el3_set_xcvr(bp, EL3_PORT_COAX);        /* no hub: try coax */
    } else if (bp->tickCarrierErrs != 0) {
        el3_set_xcvr(bp, EL3_PORT_TP);              /* coax is dead: back */
    }
    bp->tickCarrierErrs = 0;
}

static void el3_tick(el3_board_t *bp)
{
    UInt16 status;

    if (!bp->running)
        return;
    status = el3_rd16(bp->io, EL3_STATUS);
    if (status == 0xFFFF) {
        bp->cardGone = 1;
        return;
    }
    if (bp->cardGone || (status & ST_WindowMask) != ST_Window1) {
        /* The card lost power (sleep) or was reset: program it again. */
        if (!bp->inService)
            el3_hw_init(bp);
    } else {
        el3_update_stats(bp);
        el3_media_check(bp);
    }
    el3_service(bp);
    if (bp->cardGone)
        return;
    /* Never leave writers blocked on a TxAvailable that will not come. */
    if (bp->txWaiting && el3_rd16(bp->io, W1_TX_FREE) >= EL3_TX_ROOM) {
        EL3_CMD_WR(bp, CMD_AckIntr | ST_TxAvailable);
        bp->txWaiting = 0;
    }
    if (bp->needWake || !bp->txWaiting)
        el3_wake_writers(bp);
}

static void el3_timer_start(el3_board_t *bp, el3_stream_t *sp)
{
    mblk_t *mp = mi_timer_alloc(WR(sp->rq), 0);

    if (mp != NULL) {
        bp->timerMp = mp;
        bp->timerOwner = sp;
        mi_timer(mp, EL3_TICK_MS);
    }
}

static void el3_unqueue(queue_t *q, mblk_t *target, int *found)
{
    mblk_t *mp;

    for (mp = q->q_first; mp != NULL; mp = mp->b_next) {
        if (mp == target) {
            rmvq(q, mp);
            *found = 1;
            return;
        }
    }
}

static void el3_timer_stop(el3_board_t *bp)
{
    mblk_t *mp = bp->timerMp;
    int found = 0;

    if (mp == NULL)
        return;
    /* If it has already fired and is sitting on a queue, take it back. */
    el3_unqueue(WR(bp->timerOwner->rq), mp, &found);
    if (!found)
        el3_unqueue(bp->timerOwner->rq, mp, &found);
    if (found)
        (void)mi_timer_valid(mp);
    mi_timer_free(mp);
    bp->timerMp = NULL;
    bp->timerOwner = NULL;
}

/* Returns 1 if mp is our watchdog timer message (and handles it). */
static int el3_timer_msg(el3_board_t *bp, mblk_t *mp)
{
    if (mp != bp->timerMp)
        return 0;
    if (mi_timer_valid(mp)) {
        el3_tick(bp);
        mi_timer(mp, EL3_TICK_MS);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler (primary interrupt level)                        */
/* ------------------------------------------------------------------ */

static InterruptMemberNumber el3_isr(InterruptSetMember member, void *refCon,
                                     UInt32 intCount)
{
    el3_board_t *bp = (el3_board_t *)refCon;
    UInt16 status;
    queue_t *q;
    mblk_t *mp;

#ifndef EL3_HOST_TEST
#pragma unused(member, intCount)
#endif

    if (bp == NULL || !bp->running)
        return kIsrIsNotComplete;
    status = el3_rd16(bp->io, EL3_STATUS);
    if (status == 0xFFFF || !(status & ST_IntLatch))
        return kIsrIsNotComplete;

    /* Silence the card; the read service routine does the real work. */
    EL3_CMD_WR(bp, CMD_SetIntrEnb | 0);
    EL3_CMD_WR(bp, CMD_AckIntr | ST_IntReq | ST_IntLatch);
    bp->isrMasked = 1;

    OTEnterInterrupt();
    q = bp->svcQ;
    if (q != NULL && (mp = allocb(4, BPRI_HI)) != NULL) {
        mp->b_datap->db_type = M_PCSIG;
        putq(q, mp);
    } else
        bp->workPending = 1;        /* the watchdog will pick it up */
    OTLeaveInterrupt();

    return kIsrIsComplete;
}

/* ------------------------------------------------------------------ */
/* DLPI template hardware callbacks                                   */
/* ------------------------------------------------------------------ */

static void el3_hw_start(void *boardvp)
{
    el3_board_t *bp = (el3_board_t *)boardvp;

    bp->running = 1;
    bp->inService = 0;
    el3_hw_init(bp);
    if (bp->intEnabler != NULL)
        (*bp->intEnabler)(bp->ist.setID, bp->ist.member);
}

static void el3_hw_stop(void *boardvp)
{
    el3_board_t *bp = (el3_board_t *)boardvp;
    UInt16 status;

    if (!bp->running)
        return;
    bp->running = 0;        /* the interrupt handler now ignores the card */

    status = el3_rd16(bp->io, EL3_STATUS);
    if (status == 0xFFFF)
        return;
    EL3_CMD_WR(bp, CMD_SetIntrEnb | 0);
    EL3_CMD_WR(bp, CMD_AckIntr | 0xFF);
    EL3_CMD_WR(bp, CMD_StatsDisable);
    EL3_CMD_WR(bp, CMD_RxDisable);
    EL3_CMD_WR(bp, CMD_TxDisable);
    EL3_CMD_WR(bp, CMD_StopCoax);
    EL3_WINDOW(bp, 4);
    el3_wr16(bp->io, W4_MEDIA, 0);
    EL3_WINDOW(bp, 0);
    el3_wr16(bp->io, W0_RESOURCE_CONFIG, 0x0F00);
}

static void el3_hw_address_filter_reset(void *boardvp, dle_addr_t *addr_list,
                    UInt32 addr_count, UInt32 promisc_count,
                    UInt32 multi_promisc_count, UInt32 accept_broadcast,
                    UInt32 accept_errors)
{
    el3_board_t *bp = (el3_board_t *)boardvp;
    dle_addr_t *dlea;
    UInt8 *first_phys = NULL;
    UInt32 phys_count = 0, multi_count = 0;
    UInt16 filter = RXF_Station | RXF_Broadcast;
    int i, changed = 0;

#ifndef EL3_HOST_TEST
#pragma unused(addr_count, accept_broadcast, accept_errors)
#endif

    for (dlea = addr_list; dlea != NULL; dlea = dlea->dlea_next) {
        if (dlea->dlea_addr[0] & 1)
            multi_count++;
        else {
            if (first_phys == NULL)
                first_phys = dlea->dlea_addr;
            phys_count++;
        }
    }

    /* The chip has no multicast hash: it is all multicasts or none. */
    if (multi_count != 0 || multi_promisc_count != 0)
        filter |= RXF_Multicast;
    if (promisc_count != 0 || phys_count > 1)
        filter |= RXF_Multicast | RXF_Promiscuous;

    if (first_phys != NULL) {
        for (i = 0; i < 6; i++) {
            if (bp->dle.dle_current_addr[i] != first_phys[i]) {
                bp->dle.dle_current_addr[i] = first_phys[i];
                changed = 1;
            }
        }
    }

    bp->rxFilter = filter;
    if (!bp->running || bp->cardGone)
        return;             /* el3_hw_init applies it later */
    if (changed) {
        EL3_WINDOW(bp, 2);
        for (i = 0; i < 6; i++)
            el3_wr8(bp->io, i, bp->dle.dle_current_addr[i]);
        EL3_WINDOW(bp, 1);
    }
    EL3_CMD_WR(bp, CMD_SetRxFilter | filter);
}

/* ------------------------------------------------------------------ */
/* STREAMS entry points                                               */
/* ------------------------------------------------------------------ */

static int el3_open(queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp)
{
    el3_board_t *bp = el3_global;
    el3_stream_t *sp;
    int isNew, ret;

    if (bp == NULL)
        return ENXIO;
    isNew = (q->q_ptr == NULL);
    ret = dle_open(&bp->dle, q, devp, flag, sflag, credp, sizeof(el3_stream_t));
    if (ret == 0 && isNew && q->q_ptr != NULL) {
        sp = (el3_stream_t *)q->q_ptr;
        sp->rq = q;
        sp->next = bp->streams;
        bp->streams = sp;
        if (bp->svcQ == NULL)
            bp->svcQ = q;
        if (bp->timerMp == NULL)
            el3_timer_start(bp, sp);
    }
    return ret;
}

static int el3_close(queue_t *q, int flag, cred_t *credp)
{
    el3_board_t *bp = el3_global;
    el3_stream_t *sp = (el3_stream_t *)q->q_ptr;
    el3_stream_t **spp;
    int ret;

#ifndef EL3_HOST_TEST
#pragma unused(flag, credp)
#endif

    if (bp != NULL && sp != NULL) {
        for (spp = &bp->streams; *spp != NULL; spp = &(*spp)->next) {
            if (*spp == sp) {
                *spp = sp->next;
                break;
            }
        }
        /* Point the interrupt handler at a queue that will survive. */
        if (bp->svcQ == q)
            bp->svcQ = (bp->streams != NULL) ? bp->streams->rq : NULL;
        if (bp->timerOwner == sp) {
            el3_timer_stop(bp);
            if (bp->streams != NULL)
                el3_timer_start(bp, bp->streams);
        }
    }

    ret = dle_close(q);

    /* A kick queued on the stream that just went away is lost: do its work. */
    if (bp != NULL && bp->running)
        el3_service(bp);
    return ret;
}

/* Read service: interrupt kicks, template control messages, inbound data. */
static int el3_rsrv(queue_t *q)
{
    el3_board_t *bp = el3_global;
    mblk_t *mp;

    while ((mp = getq(q)) != NULL) {
        switch (mp->b_datap->db_type) {
        case M_PCSIG:
            if (bp != NULL && el3_timer_msg(bp, mp))
                break;
            freemsg(mp);
            if (bp != NULL)
                el3_service(bp);
            break;
        case M_CTL:
            /* Private template message (802.2 XID / TEST handling). */
            dle_rsrv_ctl(q, mp);
            break;
        default:
            if (canputnext(q))
                putnext(q, mp);
            else {
                /* Upstream is full: drop the data, keep servicing. */
                freemsg(mp);
                flushq(q, FLUSHDATA);
            }
            break;
        }
    }
    return 0;
}

static int el3_wput(queue_t *q, mblk_t *mp)
{
    el3_board_t *bp = el3_global;

    if (bp == NULL) {
        freemsg(mp);
        return 0;
    }
    if (mp->b_datap->db_type == M_PCSIG) {
        if (!el3_timer_msg(bp, mp))
            freemsg(mp);
        return 0;
    }
    if (mp->b_datap->db_type != M_DATA) {
        mp = dle_wput(q, mp);
        if (mp == NULL)
            return 0;
        if (mp->b_datap->db_type != M_DATA) {
            /* The template formatted a reply (or NAK'd a private ioctl). */
            qreply(q, mp);
            return 0;
        }
    }

    /* Keep frames in order: never overtake ones already waiting. */
    if (q->q_first != NULL || !el3_xmit(bp, mp))
        putq(q, mp);
    return 0;
}

static int el3_wsrv(queue_t *q)
{
    el3_board_t *bp = el3_global;
    mblk_t *mp;

    while ((mp = getq(q)) != NULL) {
        if (bp == NULL) {
            freemsg(mp);
            continue;
        }
        if (mp->b_datap->db_type == M_PCSIG) {
            if (!el3_timer_msg(bp, mp))
                freemsg(mp);
            continue;
        }
        if (!el3_xmit(bp, mp)) {
            putbq(q, mp);       /* TxAvailable will re-enable this queue */
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open Transport module exports                                      */
/* ------------------------------------------------------------------ */

install_info *GetOTInstallInfo(void)
{
    return &el3_install_info;
}

OTResult ValidateHardware(RegEntryID *our_id)
{
#ifndef EL3_HOST_TEST
#pragma unused(our_id)
#endif
    return kOTNoError;
}

Boolean InitStreamModule(void *systemDependent)
{
    RegEntryID *our_id = (RegEntryID *)systemDependent;
    el3_board_t *bp;
    dle_t *dle;
    RegPropertyValueSize size;
    LogicalAddress addrs[8];
    InterruptSetMember ist[kISTPropertyMemberCount];
    UInt16 w, status;
    int i;

    if (our_id == NULL || el3_global != NULL)
        return false;

    bp = (el3_board_t *)OTAllocMem(sizeof(el3_board_t));
    if (bp == NULL)
        return false;
    bzero((char *)bp, sizeof(el3_board_t));
    dle = &bp->dle;

    /* Where PC Card Manager mapped the card's I/O ports. */
    size = sizeof(addrs);
    if (RegistryPropertyGet(our_id, kAAPLDeviceLogicalAddress, addrs, &size) != noErr
        || size < sizeof(LogicalAddress) || addrs[0] == NULL)
        goto fail;
    bp->io = (void *)addrs[0];

    size = sizeof(ist);
    if (RegistryPropertyGet(our_id, kISTPropertyName, ist, &size) != noErr
        || size < sizeof(InterruptSetMember))
        goto fail;
    bp->ist = ist[kISTChipInterruptSource];

    /* Is an EtherLink III really answering there? */
    EL3_CMD_WR(bp, CMD_SetIntrEnb | 0);
    EL3_WINDOW(bp, 0);
    status = el3_rd16(bp->io, EL3_STATUS);
    if (status == 0xFFFF || (status & ST_WindowMask) != 0)
        goto fail;

    /* The station address is in the first three EEPROM words. */
    for (i = 0; i < 3; i++) {
        w = el3_read_eeprom(bp, i);
        dle->dle_factory_addr[i * 2]     = (UInt8)(w >> 8);
        dle->dle_factory_addr[i * 2 + 1] = (UInt8)w;
    }
    w = 0;
    for (i = 0; i < 6; i++)
        w |= dle->dle_factory_addr[i];
    if (w == 0 || (dle->dle_factory_addr[0] & 1))
        goto fail;          /* blank EEPROM or nonsense address */
    for (i = 0; i < 6; i++)
        dle->dle_current_addr[i] = dle->dle_factory_addr[i];

    bp->ifPort = EL3_DEFAULT_PORT;
    bp->curPort = EL3_PORT_TP;
    bp->rxFilter = RXF_Station | RXF_Broadcast;

    dle->dle_hw.dlehw_start = el3_hw_start;
    dle->dle_hw.dlehw_stop = el3_hw_stop;
    dle->dle_hw.dlehw_address_filter_reset = el3_hw_address_filter_reset;
    dle->dle_hw.dlehw_send_error = NULL;
    dle->dle_hw.dlehw_recv_error_flags = 0;

    /* Take over the card's interrupt; it stays masked on the card itself. */
    if (GetInterruptFunctions(bp->ist.setID, bp->ist.member, &bp->oldRefCon,
            &bp->oldHandler, &bp->intEnabler, &bp->intDisabler) != noErr)
        goto fail;
    if (InstallInterruptFunctions(bp->ist.setID, bp->ist.member, bp, el3_isr,
            bp->intEnabler, bp->intDisabler) != noErr)
        goto fail;
    bp->isrInstalled = 1;

    /* From here on dle_terminate must be called before freeing. */
    dle_init(dle, 0);
    el3_global = bp;
    return true;

fail:
    OTFreeMem(bp);
    return false;
}

void TerminateStreamModule(void)
{
    el3_board_t *bp = el3_global;

    if (bp == NULL)
        return;
    el3_hw_stop(bp);
    if (bp->isrInstalled)
        (void)InstallInterruptFunctions(bp->ist.setID, bp->ist.member,
                bp->oldRefCon, bp->oldHandler, bp->intEnabler, bp->intDisabler);
    el3_global = NULL;
    dle_terminate(&bp->dle);
    OTFreeMem(bp);
}
