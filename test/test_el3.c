/*
 * test_el3.c - host-side test of EL3PCCard.c.
 *
 * Contains (1) a register-level model of the 3C589, (2) a tiny mock of the
 * Open Transport STREAMS / DLPI-template environment and (3) the tests.
 *
 * The chip model decodes registers per window and asserts on anything the
 * driver should never do (touching a windowed register from the interrupt
 * handler, overfilling the TX FIFO, reading the RX FIFO with no frame...).
 *
 * This checks the driver's logic against our understanding of the chip
 * and of Open Transport.  It is not a substitute for testing on a Mac.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "host_stubs.h"
#include "../src/EL3Reg.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

install_info *GetOTInstallInfo(void);
Boolean InitStreamModule(void *);
void TerminateStreamModule(void);

/* ================================================================== */
/* Chip model                                                         */
/* ================================================================== */

#define SIM_TXCAP   2048
#define SIM_MAXQ    64

typedef struct { UInt8 d[1600]; int len; int err; } frame_t;

static struct {
    int gone, edgeLatch, inIsr;
    int window, intMask, statusEnb, status, latch;
    int rxEnabled, txEnabled, statsEnabled, activated, resCfg, addrCfg, coax, media, link;
    UInt16 eeprom[3]; int eeIndex;
    UInt8 station[6];
    int rxFilter;
    frame_t rxq[SIM_MAXQ]; int rxHead, rxCount, rxOff;
    int txFree, txThresh, txState, txLen, txGot, txHold;
    frame_t cur;
    frame_t sent[SIM_MAXQ]; int sentCount;
    UInt8 txStatus[8]; int txStatusCount;
    UInt8 stats[9];
    int resets;
} sim;

static void sim_eval(int newEvent)
{
    int pend = sim.status & sim.intMask & 0xFE;
    if (sim.edgeLatch ? (newEvent && pend) : pend)
        sim.latch = 1;
}

static void sim_set(int bit) { sim.status |= bit; sim_eval(1); }

static void sim_power_cycle(void)
{
    UInt16 ee[3]; int edge = sim.edgeLatch, link = sim.link, resets = sim.resets;
    memcpy(ee, sim.eeprom, sizeof ee);
    memset(&sim, 0, sizeof sim);
    memcpy(sim.eeprom, ee, sizeof ee);
    sim.edgeLatch = edge; sim.link = link; sim.resets = resets + 1;
    sim.txFree = SIM_TXCAP;
}

static void sim_inject(const UInt8 *d, int len, int err)
{
    frame_t *f;
    if (!sim.rxEnabled || sim.rxCount == SIM_MAXQ) return;
    f = &sim.rxq[(sim.rxHead + sim.rxCount++) % SIM_MAXQ];
    memcpy(f->d, d, len); f->len = len; f->err = err;
    sim_set(ST_RxComplete);
}

/* The wire finishes sending everything in the TX FIFO. */
static void sim_tx_drain(void)
{
    sim.txFree = SIM_TXCAP;
    if (sim.txThresh && sim.txFree >= sim.txThresh) { sim.txThresh = 0; sim_set(ST_TxAvailable); }
}

static void sim_tx_error(UInt8 st)
{
    sim.txStatus[sim.txStatusCount++] = st | TXS_Complete;
    sim.txEnabled = 0;
    sim_set(ST_TxComplete);
}

static void sim_cmd(UInt16 v)
{
    int op = v >> 11, arg = v & 0x7FF;
    switch (op) {
    case 1: sim.window = arg & 7; break;
    case 2: sim.coax = 1; break;
    case 3: sim.rxEnabled = 0; break;
    case 4: sim.rxEnabled = 1; break;
    case 5: sim.rxEnabled = 0; sim.rxCount = 0; sim.rxOff = 0; sim.status &= ~ST_RxComplete; break;
    case 8:
        CHECK(sim.rxCount > 0);
        sim.rxHead = (sim.rxHead + 1) % SIM_MAXQ; sim.rxCount--; sim.rxOff = 0;
        if (!sim.rxCount) sim.status &= ~ST_RxComplete;
        break;
    case 9: sim.txEnabled = 1; break;
    case 10: sim.txEnabled = 0; break;
    case 11: sim.txEnabled = 0; sim.txFree = SIM_TXCAP; sim.txState = 0; sim.txStatusCount = 0;
             sim.status &= ~ST_TxComplete; break;
    case 13:
        sim.status &= ~(arg & (ST_TxAvailable | ST_RxEarly | ST_IntReq | ST_AdapterFailure));
        if (arg & ST_IntLatch) { sim.latch = 0; sim_eval(0); }
        break;
    case 14: sim.intMask = arg & 0xFF; sim_eval(0); break;
    case 15: sim.statusEnb = arg & 0xFF; break;
    case 16: sim.rxFilter = arg & 0xF; break;
    case 18:
        sim.txThresh = arg;
        if (sim.txFree >= arg) { sim.txThresh = 0; sim_set(ST_TxAvailable); }
        break;
    case 21: sim.statsEnabled = 1; break;
    case 22: sim.statsEnabled = 0; break;
    case 23: sim.coax = 0; break;
    default: printf("sim: unexpected command %d\n", op); exit(1);
    }
}

static void sim_tx_byte(UInt8 b)
{
    CHECK(sim.txState == 2);
    if (sim.txGot < sim.txLen) sim.cur.d[sim.txGot] = b;
    sim.txGot++;
    if (sim.txGot == ((sim.txLen + 3) & ~3)) {
        sim.cur.len = sim.txLen;
        CHECK(sim.txEnabled);
        CHECK(sim.sentCount < SIM_MAXQ);
        sim.sent[sim.sentCount++] = sim.cur;
        sim.txState = 0;
        if (!sim.txHold) sim_tx_drain();
    }
}

UInt16 el3_rd16(void *io, int off)
{
    UInt16 v;
    (void)io;
    if (sim.gone) return 0xFFFF;
    if (off == EL3_STATUS)
        return (UInt16)((sim.window << 13) | (sim.status & sim.statusEnb & 0xFE) |
                        (sim.status & ST_TxAvailable) | sim.latch);
    CHECK(!sim.inIsr);
    switch (sim.window * 16 + off) {
    case 0 * 16 + W0_EEPROM_CMD:    return 0;
    case 0 * 16 + W0_EEPROM_DATA:   return sim.eeprom[sim.eeIndex];
    case 1 * 16 + W1_RX_STATUS:
        if (!sim.rxCount) return RXS_Incomplete;
        return (UInt16)(sim.rxq[sim.rxHead].len | (sim.rxq[sim.rxHead].err ? RXS_Error | 0x2800 : 0));
    case 1 * 16 + W1_TX_FREE:       return (UInt16)sim.txFree;
    case 4 * 16 + W4_FIFO_DIAG:     return 0;
    case 4 * 16 + W4_MEDIA:         return (UInt16)(sim.media | (sim.link ? MEDIA_LinkBeat : 0));
    case 6 * 16 + W6_RX_BYTES_OK:
    case 6 * 16 + W6_TX_BYTES_OK:   CHECK(!sim.statsEnabled); return 0;
    }
    printf("sim: bad 16-bit read, window %d offset %d\n", sim.window, off); exit(1);
    return v;
}

void el3_wr16(void *io, int off, UInt16 v)
{
    (void)io;
    if (sim.gone) return;
    if (off == EL3_CMD) { sim_cmd(v); return; }
    CHECK(!sim.inIsr);
    switch (sim.window * 16 + off) {
    case 0 * 16 + W0_CONFIG_CTRL:       sim.activated = v & 1; return;
    case 0 * 16 + W0_ADDR_CONFIG:       sim.addrCfg = v; return;
    case 0 * 16 + W0_RESOURCE_CONFIG:   sim.resCfg = v; return;
    case 0 * 16 + W0_EEPROM_CMD:        CHECK((v & 0xC0) == EEPROM_READ); sim.eeIndex = v & 0x3F; CHECK(sim.eeIndex < 3); return;
    case 1 * 16 + W1_TX_FIFO:
        if (sim.txState == 0) {
            sim.txLen = v; sim.txGot = 0; sim.txState = 1;
            CHECK(v >= 60 && v <= 1514);
            CHECK(sim.txFree >= ((v + 3) & ~3) + 4);
            sim.txFree -= ((v + 3) & ~3) + 4;
        } else { CHECK(sim.txState == 1); CHECK(v == 0); sim.txState = 2; }
        return;
    case 4 * 16 + W4_MEDIA:             sim.media = v & 0xC1; return;
    }
    printf("sim: bad 16-bit write, window %d offset %d\n", sim.window, off); exit(1);
}

UInt8 el3_rd8(void *io, int off)
{
    UInt8 v;
    (void)io;
    if (sim.gone) return 0xFF;
    CHECK(!sim.inIsr);
    if (sim.window == 1 && off == W1_TX_STATUS)
        return sim.txStatusCount ? sim.txStatus[0] : 0;
    if (sim.window == 6 && off < 9) {
        CHECK(!sim.statsEnabled);
        v = sim.stats[off]; sim.stats[off] = 0;
        sim.status &= ~ST_StatsFull;
        return v;
    }
    printf("sim: bad 8-bit read, window %d offset %d\n", sim.window, off); exit(1);
    return 0;
}

void el3_wr8(void *io, int off, UInt8 v)
{
    (void)io;
    if (sim.gone) return;
    CHECK(!sim.inIsr);
    if (sim.window == 2 && off < 6) { sim.station[off] = v; return; }
    if (sim.window == 1 && off == W1_TX_STATUS) {
        if (sim.txStatusCount > 0)      /* popping an empty stack is harmless */
            memmove(sim.txStatus, sim.txStatus + 1, --sim.txStatusCount);
        if (!sim.txStatusCount) sim.status &= ~ST_TxComplete;
        return;
    }
    printf("sim: bad 8-bit write, window %d offset %d\n", sim.window, off); exit(1);
}

void el3_fifo_write(void *io, const UInt8 *p, UInt32 words)
{
    (void)io;
    CHECK(((size_t)p & 1) == 0);
    if (sim.gone) return;
    CHECK(!sim.inIsr && sim.window == 1);
    while (words--) { sim_tx_byte(p[0]); sim_tx_byte(p[1]); p += 2; }
}

void el3_fifo_read(void *io, UInt8 *p, UInt32 words)
{
    frame_t *f = &sim.rxq[sim.rxHead];
    (void)io;
    CHECK(((size_t)p & 1) == 0);
    CHECK(!sim.gone && !sim.inIsr && sim.window == 1 && sim.rxCount > 0);
    while (words--) {
        p[0] = sim.rxOff < f->len ? f->d[sim.rxOff] : 0xEE; sim.rxOff++;
        p[1] = sim.rxOff < f->len ? f->d[sim.rxOff] : 0xEE; sim.rxOff++;
        p += 2;
    }
}

/* ================================================================== */
/* Mock Mac OS / Open Transport                                       */
/* ================================================================== */

static InterruptHandler g_isr; static void *g_isrRefCon;
static int g_liveMsgs, g_allocFail, g_canput = 1, g_inInterrupt;
static queue_t *g_queues[16]; static int g_nq;
static dle_t *g_dle; static queue_t *g_open[8]; static int g_nopen;
static frame_t g_up[SIM_MAXQ]; static int g_upCount;
static mblk_t *g_timer; static queue_t *g_timerQ; static int g_timerArmed, g_timerFired;

OSStatus RegistryPropertyGet(const RegEntryID *id, const char *name, void *buf, RegPropertyValueSize *size)
{
    (void)id;
    if (!strcmp(name, kAAPLDeviceLogicalAddress)) {
        LogicalAddress a = (LogicalAddress)0x1000; memcpy(buf, &a, sizeof a); *size = sizeof a; return noErr;
    }
    if (!strcmp(name, kISTPropertyName)) {
        InterruptSetMember m[3]; memset(m, 0, sizeof m); m[0].member = 5;
        memcpy(buf, m, sizeof m); *size = sizeof m; return noErr;
    }
    return -1;
}
OSStatus GetInterruptFunctions(InterruptSetID s, InterruptMemberNumber m, void **rc, InterruptHandler *h, InterruptEnabler *e, InterruptDisabler *d)
{ (void)s; (void)m; *rc = NULL; *h = NULL; *e = NULL; *d = NULL; return noErr; }
OSStatus InstallInterruptFunctions(InterruptSetID s, InterruptMemberNumber m, void *rc, InterruptHandler h, InterruptEnabler e, InterruptDisabler d)
{ (void)s; (void)m; (void)e; (void)d; g_isr = h; g_isrRefCon = rc; return noErr; }

void *OTAllocMem(size_t n) { return malloc(n); }
void OTFreeMem(void *p) { free(p); }
void OTEnterInterrupt(void) { g_inInterrupt++; }
void OTLeaveInterrupt(void) { g_inInterrupt--; }

queue_t *WR(queue_t *q) { return q->t_isWrite ? q : q->t_other; }
queue_t *RD(queue_t *q) { return q->t_isWrite ? q->t_other : q; }

mblk_t *allocb(size_t size, int pri)
{
    mblk_t *mp; (void)pri;
    if (g_allocFail == 1 || (g_allocFail == 2 && size > 4)) return NULL;
    mp = calloc(1, sizeof *mp + sizeof(struct datab) + size + 8);
    mp->b_datap = (struct datab *)(mp + 1);
    mp->b_datap->db_base = (UInt8 *)(mp->b_datap + 1);
    mp->b_datap->db_lim = mp->b_datap->db_base + size;
    mp->b_rptr = mp->b_wptr = mp->b_datap->db_base;
    g_liveMsgs++;
    return mp;
}
void freemsg(mblk_t *mp)
{
    while (mp) { mblk_t *n = mp->b_cont; CHECK(mp != g_timer); free(mp); g_liveMsgs--; mp = n; }
}
int putq(queue_t *q, mblk_t *mp)
{
    CHECK(!g_inInterrupt || !q->t_isWrite);     /* ISR may only putq on a read queue */
    mp->b_next = NULL;
    if (mp->b_datap->db_type >= 0x80) { mp->b_next = q->q_first; q->q_first = mp; if (!q->q_last) q->q_last = mp; }
    else { if (q->q_last) q->q_last->b_next = mp; else q->q_first = mp; q->q_last = mp; }
    q->t_enabled = 1;
    return 1;
}
int putbq(queue_t *q, mblk_t *mp)
{ mp->b_next = q->q_first; q->q_first = mp; if (!q->q_last) q->q_last = mp; return 1; }
mblk_t *getq(queue_t *q)
{
    mblk_t *mp = q->q_first;
    if (mp) { q->q_first = mp->b_next; if (!q->q_first) q->q_last = NULL; mp->b_next = NULL; }
    return mp;
}
void rmvq(queue_t *q, mblk_t *mp)
{
    mblk_t **pp, *prev = NULL;
    for (pp = &q->q_first; *pp; prev = *pp, pp = &(*pp)->b_next)
        if (*pp == mp) { *pp = mp->b_next; if (q->q_last == mp) q->q_last = prev; mp->b_next = NULL; return; }
    CHECK(0);
}
void qenable(queue_t *q) { CHECK(!g_inInterrupt); q->t_enabled = 1; }
void flushq(queue_t *q, int flag)
{
    mblk_t *mp, *keep = NULL, *kl = NULL; (void)flag;
    while ((mp = getq(q))) {
        if (mp->b_datap->db_type < 0x80 && mp->b_datap->db_type != M_CTL) freemsg(mp);
        else { if (kl) kl->b_next = mp; else keep = mp; kl = mp; }
    }
    q->q_first = keep; q->q_last = kl;
}
int canputnext(queue_t *q) { (void)q; CHECK(!g_inInterrupt); return g_canput; }
void putnext(queue_t *q, mblk_t *mp)
{
    frame_t *f = &g_up[g_upCount++]; (void)q;
    CHECK(g_upCount <= SIM_MAXQ);
    f->len = (int)(mp->b_wptr - mp->b_rptr); memcpy(f->d, mp->b_rptr, f->len);
    freemsg(mp);
}
void qreply(queue_t *q, mblk_t *mp) { (void)q; freemsg(mp); }

mblk_t *mi_timer_alloc(queue_t *q, size_t size)
{
    CHECK(g_timer == NULL); CHECK(q->t_isWrite);
    g_timer = allocb(size, 0); g_liveMsgs--; g_timer->b_datap->db_type = M_PCSIG; g_timerQ = q;
    return g_timer;
}
void mi_timer(mblk_t *mp, unsigned long ms) { CHECK(mp == g_timer); CHECK(ms > 0); g_timerArmed = 1; }
Boolean mi_timer_valid(mblk_t *mp) { CHECK(mp == g_timer); if (g_timerFired) { g_timerFired = 0; return true; } return false; }
void mi_timer_free(mblk_t *mp)
{ CHECK(mp == g_timer); CHECK(!g_timerFired); free(mp); g_timer = NULL; g_timerQ = NULL; g_timerArmed = 0; }

void dle_init(dle_t *dle, size_t xtra) { (void)xtra; g_dle = dle; }
void dle_terminate(dle_t *dle) { CHECK(dle == g_dle); g_dle = NULL; }
int dle_open(dle_t *dle, queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp, size_t dcl_len)
{
    (void)devp; (void)flag; (void)sflag; (void)credp;
    q->q_ptr = WR(q)->q_ptr = calloc(1, dcl_len);
    g_open[g_nopen++] = q;
    if (dle->dle_refcnt++ == 0) dle->dle_hw.dlehw_start(dle);
    return 0;
}
int dle_close(queue_t *q)
{
    int i; mblk_t *mp;
    for (i = 0; i < g_nopen; i++) if (g_open[i] == q) { g_open[i] = g_open[--g_nopen]; break; }
    while ((mp = getq(q))) freemsg(mp);
    while ((mp = getq(WR(q)))) freemsg(mp);
    q->t_enabled = WR(q)->t_enabled = 0;
    free(q->q_ptr); q->q_ptr = WR(q)->q_ptr = NULL;
    if (--g_dle->dle_refcnt == 0) g_dle->dle_hw.dlehw_stop(g_dle);
    return 0;
}
void dle_inbound(dle_t *dle, mblk_t *mp) { CHECK(dle == g_dle); CHECK(g_nopen > 0); putq(g_open[0], mp); }
void dle_rsrv_ctl(queue_t *q, mblk_t *mp) { (void)q; freemsg(mp); }
mblk_t *dle_wput(queue_t *q, mblk_t *mp) { (void)q; freemsg(mp); return NULL; }

/* ================================================================== */
/* Harness                                                            */
/* ================================================================== */

static struct streamtab *g_tab;
static int g_isrCalls;

static queue_t *stream_open(void)
{
    queue_t *pair = calloc(2, sizeof(queue_t));
    pair[0].q_qinfo = g_tab->st_rdinit; pair[0].t_other = &pair[1];
    pair[1].q_qinfo = g_tab->st_wrinit; pair[1].t_other = &pair[0]; pair[1].t_isWrite = 1;
    g_queues[g_nq++] = &pair[0]; g_queues[g_nq++] = &pair[1];
    CHECK(g_tab->st_rdinit->qi_qopen(&pair[0], NULL, 0, 0, NULL) == 0);
    return &pair[0];
}

/* Deliver interrupts and run service routines until everything is quiet. */
static void pump(void)
{
    int i, busy, guard = 0;
    do {
        busy = 0;
        CHECK(++guard < 1000);
        if (!sim.gone && sim.latch && g_isr) {
            InterruptSetMember m = { 0, 5 };
            sim.inIsr = 1; g_isr(m, g_isrRefCon, 1); sim.inIsr = 0;
            g_isrCalls++; busy = 1;
            CHECK(!sim.latch);          /* handler must silence the card */
        }
        for (i = 0; i < g_nq; i++) {
            queue_t *q = g_queues[i];
            if (q->t_enabled && q->q_ptr) { q->t_enabled = 0; q->q_qinfo->qi_srvp(q); busy = 1; }
        }
    } while (busy);
    if (!sim.gone) CHECK(sim.window == 1 || !sim.rxEnabled);
}

static void stream_close(queue_t *q)
{
    int i;
    CHECK(g_tab->st_rdinit->qi_qclose(q, 0, NULL) == 0);
    for (i = 0; i < g_nq; i++) if (g_queues[i] == q) { g_queues[i] = g_queues[g_nq - 2]; g_queues[i + 1] = g_queues[g_nq - 1]; g_nq -= 2; break; }
    free(q);
}

static void fire_timer(void)
{
    CHECK(g_timer && g_timerArmed);
    g_timerArmed = 0; g_timerFired = 1;
    g_timerQ->q_qinfo->qi_putp(g_timerQ, g_timer);
    CHECK(g_timerArmed);        /* handler must re-arm */
    pump();
}

static void make_frame(UInt8 *d, int len, int seed)
{
    int i;
    for (i = 0; i < len; i++) d[i] = (UInt8)(seed * 31 + i * 7 + 1);
    d[0] = 0x00; d[12] = 0x08; d[13] = 0x00;
}

/* Send a frame split into a chain of mblks of the given piece size. */
static void send(queue_t *q, const UInt8 *d, int len, int piece)
{
    mblk_t *head = NULL, **pp = &head;
    int off = 0, n;
    while (off < len) {
        mblk_t *mp;
        n = len - off < piece ? len - off : piece;
        mp = allocb(n, 0); memcpy(mp->b_wptr, d + off, n); mp->b_wptr += n;
        *pp = mp; pp = &mp->b_cont; off += n;
    }
    WR(q)->q_qinfo->qi_putp(WR(q), head);
}

int main(void)
{
    static const UInt8 mac[6] = { 0x00, 0x60, 0x97, 0x12, 0x34, 0x56 };
    UInt8 f[1600], g[1600];
    queue_t *a, *b;
    int i, n, before;
    RegEntryID id;

    memset(&id, 0, sizeof id);
    sim_power_cycle();
    sim.eeprom[0] = 0x0060; sim.eeprom[1] = 0x9712; sim.eeprom[2] = 0x3456;
    sim.link = 1;

    /* --- probe: absent card is refused, present card yields its address --- */
    sim.gone = 1; CHECK(!InitStreamModule(&id)); sim.gone = 0;
    CHECK(InitStreamModule(&id));
    CHECK(g_dle && !memcmp(g_dle->dle_factory_addr, mac, 6) && !memcmp(g_dle->dle_current_addr, mac, 6));
    CHECK(g_isr != NULL && sim.intMask == 0);
    g_tab = GetOTInstallInfo()->install_str;
    CHECK(!strcmp(g_tab->st_rdinit->qi_minfo->mi_idname, "pccard101,589"));

    /* --- start --- */
    a = stream_open(); pump();
    CHECK(sim.rxEnabled && sim.txEnabled && sim.activated && sim.resCfg == 0x3F00);
    CHECK(!memcmp(sim.station, mac, 6));
    CHECK(sim.rxFilter == (RXF_Station | RXF_Broadcast));
    CHECK((sim.intMask & ST_RxComplete) && !(sim.intMask & ST_TxAvailable));
    CHECK(sim.addrCfg == XCVR_TP && (sim.media & MEDIA_TP) && g_timerArmed);
    printf("ok  probe/start\n");

    /* --- receive: odd, even, minimum and maximum lengths, in order --- */
    {
        static const int lens[] = { 60, 61, 62, 63, 64, 1513, 1514 };
        for (i = 0; i < 7; i++) { make_frame(f, lens[i], i); sim_inject(f, lens[i], 0); }
        sim_inject(f, 100, 1);                      /* CRC error: must be dropped */
        pump();
        CHECK(g_upCount == 7 && sim.rxCount == 0);
        for (i = 0; i < 7; i++) {
            make_frame(f, lens[i], i);
            CHECK(g_up[i].len == lens[i] && !memcmp(g_up[i].d, f, lens[i]));
        }
        CHECK(g_dle->dle_estatus.receive_fcs_errors == 1);
        g_upCount = 0;
    }
    /* a burst larger than one service pass still gets fully delivered */
    for (i = 0; i < 50; i++) { make_frame(f, 200 + i, i); sim_inject(f, 200 + i, 0); }
    pump(); CHECK(g_upCount == 50 && g_up[49].len == 249); g_upCount = 0;
    /* no memory for the interrupt kick: the watchdog still finds the frame */
    g_allocFail = 1; make_frame(f, 100, 1); sim_inject(f, 100, 0); pump(); g_allocFail = 0;
    CHECK(g_upCount == 0 && sim.rxCount == 1);
    fire_timer();
    CHECK(sim.rxCount == 0 && g_upCount == 1); g_upCount = 0;
    /* no memory for the frame itself: dropped and counted, receiver keeps going */
    g_allocFail = 2; sim_inject(f, 100, 0); pump(); g_allocFail = 0;
    CHECK(g_upCount == 0 && sim.rxCount == 0 && g_dle->dle_istatus.receive_discards == 1);
    make_frame(f, 100, 2); sim_inject(f, 100, 0); pump(); CHECK(g_upCount == 1); g_upCount = 0;
    /* upstream flow control: data dropped, driver survives */
    g_canput = 0; sim_inject(f, 100, 0); pump(); g_canput = 1; CHECK(g_upCount == 0);
    sim_inject(f, 100, 0); pump(); CHECK(g_upCount == 1); g_upCount = 0;
    printf("ok  receive\n");

    /* --- transmit: padding, odd lengths, fragmented messages --- */
    {
        static const int lens[] = { 14, 42, 59, 60, 61, 63, 777, 1514 };
        static const int piece[] = { 14, 5, 7, 60, 3, 1, 100, 1460 };
        for (i = 0; i < 8; i++) { make_frame(f, lens[i], i); send(a, f, lens[i], piece[i]); }
        pump();
        CHECK(sim.sentCount == 8);
        for (i = 0; i < 8; i++) {
            n = lens[i] < 60 ? 60 : lens[i];
            make_frame(f, lens[i], i); memset(f + lens[i], 0, 1600 - lens[i]);
            CHECK(sim.sent[i].len == n && !memcmp(sim.sent[i].d, f, n));
        }
        sim.sentCount = 0;
    }
    /* 802.3: zero length field is filled in with the payload length */
    make_frame(f, 100, 3); f[12] = f[13] = 0; send(a, f, 100, 100); pump();
    CHECK(sim.sent[0].d[12] == 0 && sim.sent[0].d[13] == 86); sim.sentCount = 0;
    /* oversize frame is dropped, not sent */
    make_frame(f, 1515, 3); send(a, f, 1515, 500); pump();
    CHECK(sim.sentCount == 0 && g_dle->dle_istatus.transmit_errors == 1);
    printf("ok  transmit\n");

    /* --- transmit back-pressure: FIFO full, frames wait and keep order --- */
    b = stream_open();
    sim.txHold = 1;
    for (i = 0; i < 6; i++) { make_frame(f, 1000, 40 + i); send(a, f, 1000, 1000); }
    make_frame(f, 1000, 99); send(b, f, 1000, 1000);
    pump();
    CHECK(sim.sentCount == 2);                      /* 2 x 1004 fit in 2048 */
    CHECK(WR(a)->q_first != NULL && WR(b)->q_first != NULL);
    CHECK(sim.intMask & ST_TxAvailable);
    for (n = 0; n < 10 && sim.sentCount < 7; n++) { sim_tx_drain(); pump(); }
    sim.txHold = 0;
    CHECK(sim.sentCount == 7 && WR(a)->q_first == NULL && WR(b)->q_first == NULL);
    for (i = 0, n = 0; i < 7; i++) {                /* stream a's frames in order */
        make_frame(f, 1000, 40 + n);
        if (!memcmp(sim.sent[i].d, f, 1000)) n++;
    }
    CHECK(n == 6);
    sim_tx_drain(); pump();
    CHECK(!(sim.intMask & ST_TxAvailable));
    sim.sentCount = 0;
    /* lost TxAvailable interrupt: the watchdog restarts the writers */
    sim.txHold = 1;
    for (i = 0; i < 3; i++) { make_frame(f, 1000, i); send(a, f, 1000, 1000); }
    pump(); CHECK(sim.sentCount == 2);
    sim.txFree = SIM_TXCAP; sim.txThresh = 0;       /* drained, but no event */
    sim.txHold = 0; fire_timer();
    CHECK(sim.sentCount == 3); sim.sentCount = 0;
    printf("ok  transmit back-pressure\n");

    /* --- transmit error: transmitter is re-enabled --- */
    sim_tx_error(TXS_MaxCollisions); pump();
    CHECK(sim.txEnabled && sim.txStatusCount == 0);
    CHECK(g_dle->dle_estatus.transmit_excessive_collisions == 1);
    sim_tx_error(TXS_Underrun); pump();
    CHECK(sim.txEnabled && sim.txStatusCount == 0);
    make_frame(f, 64, 1); send(a, f, 64, 64); pump(); CHECK(sim.sentCount == 1); sim.sentCount = 0;
    printf("ok  transmit errors\n");

    /* --- statistics overflow interrupt --- */
    sim.stats[W6_SINGLE_COLL] = 200; sim_set(ST_StatsFull); pump();
    CHECK(g_dle->dle_estatus.transmit_single_collision_frames == 200 && !(sim.status & ST_StatsFull));
    printf("ok  statistics\n");

    /* --- address filter --- */
    {
        dle_addr_t m1, p1;
        memset(&m1, 0, sizeof m1); memset(&p1, 0, sizeof p1);
        m1.dlea_addr[0] = 0x09; m1.dlea_addr[1] = 0x00; m1.dlea_addr[2] = 0x07;
        g_dle->dle_hw.dlehw_address_filter_reset(g_dle, &m1, 1, 0, 0, 1, 0);
        CHECK(sim.rxFilter == (RXF_Station | RXF_Broadcast | RXF_Multicast));
        g_dle->dle_hw.dlehw_address_filter_reset(g_dle, NULL, 0, 1, 0, 1, 0);
        CHECK(sim.rxFilter & RXF_Promiscuous);
        memcpy(p1.dlea_addr, "\x02\x11\x22\x33\x44\x55", 6);
        g_dle->dle_hw.dlehw_address_filter_reset(g_dle, &p1, 1, 0, 0, 1, 0);
        CHECK(sim.rxFilter == (RXF_Station | RXF_Broadcast));
        CHECK(!memcmp(sim.station, p1.dlea_addr, 6) && sim.window == 1);
    }
    printf("ok  address filter\n");

    /* --- media: lose link beat -> coax; carrier errors on coax -> back --- */
    sim.link = 0; fire_timer(); fire_timer();
    CHECK(sim.addrCfg == XCVR_COAX && sim.coax);
    fire_timer(); CHECK(sim.addrCfg == XCVR_COAX);
    sim.link = 1; sim.stats[W6_CARRIER_LOST] = 1; fire_timer();
    CHECK(sim.addrCfg == XCVR_TP && !sim.coax && (sim.media & MEDIA_TP));
    fire_timer(); fire_timer(); CHECK(sim.addrCfg == XCVR_TP);
    printf("ok  media selection\n");

    /* --- sleep/wake: chip loses all state, watchdog reprograms it --- */
    sim.txHold = 1;
    for (i = 0; i < 3; i++) { make_frame(f, 1000, i); send(a, f, 1000, 1000); }
    pump(); sim.sentCount = 0;
    sim.gone = 1; fire_timer(); fire_timer();       /* socket powered off */
    make_frame(g, 80, 7); send(b, g, 80, 80); pump();
    sim.gone = 0; sim_power_cycle();
    CHECK(!sim.rxEnabled);
    fire_timer();
    CHECK(sim.rxEnabled && sim.txEnabled && sim.resCfg == 0x3F00 && (sim.intMask & ST_RxComplete));
    CHECK(!memcmp(sim.station, "\x02\x11\x22\x33\x44\x55", 6));
    CHECK(sim.sentCount == 1 && WR(a)->q_first == NULL);    /* the queued frame went out */
    make_frame(f, 90, 5); sim_inject(f, 90, 0); pump();
    CHECK(g_upCount == 1 && g_up[0].len == 90); g_upCount = 0; sim.sentCount = 0;
    printf("ok  sleep/wake recovery\n");

    /* --- edge-triggered latch model: no event may be stranded --- */
    sim.edgeLatch = 1;
    for (i = 0; i < 40; i++) { make_frame(f, 100 + i, i); sim_inject(f, 100 + i, 0); }
    pump(); CHECK(g_upCount == 40); g_upCount = 0;
    sim.edgeLatch = 0;
    printf("ok  edge-latched interrupts\n");

    /* --- close the stream the interrupt handler is kicking --- */
    make_frame(f, 70, 1); sim_inject(f, 70, 0);
    {   /* take the interrupt but do not run the service routine yet */
        InterruptSetMember m = { 0, 5 };
        CHECK(sim.latch); sim.inIsr = 1; g_isr(m, g_isrRefCon, 1); sim.inIsr = 0;
    }
    stream_close(a);                                /* kick message dies with the queue */
    pump();
    CHECK(sim.rxCount == 0 && (sim.intMask & ST_RxComplete));
    CHECK(g_timer != NULL && g_timerQ == WR(b));    /* watchdog moved to stream b */
    make_frame(f, 71, 1); sim_inject(f, 71, 0); pump();
    CHECK(g_upCount == 2 && g_up[1].len == 71); g_upCount = 0;
    printf("ok  close of service stream\n");

    /* --- hot removal while open: nothing hangs, nothing is touched --- */
    sim.gone = 1;
    make_frame(f, 64, 1); send(b, f, 64, 64); pump();
    fire_timer();
    before = g_isrCalls; pump(); CHECK(g_isrCalls == before);
    stream_close(b);
    CHECK(g_timer == NULL);
    TerminateStreamModule();
    CHECK(g_isr == NULL && g_dle == NULL);
    printf("ok  hot removal / shutdown\n");

    CHECK(g_liveMsgs == 0);
    printf("ALL TESTS PASSED\n");
    return 0;
}
