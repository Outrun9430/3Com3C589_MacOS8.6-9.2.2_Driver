/*
 * host_stubs.h - just enough of the Mac OS / Open Transport kernel
 * interfaces to compile EL3PCCard.c on a development host for testing.
 *
 * These are NOT the real headers.  Structures are minimal stand-ins that
 * carry only the members the driver uses; the real build uses Apple's
 * Universal Interfaces and the Open Transport SDK.
 */
#ifndef HOST_STUBS_H
#define HOST_STUBS_H

#include <stddef.h>
#include <string.h>

typedef unsigned char   UInt8;
typedef unsigned short  UInt16;
typedef unsigned int    UInt32;
typedef unsigned char   Boolean;
typedef int             OSStatus;
typedef int             OTResult;
typedef void            *LogicalAddress;
typedef unsigned int    RegPropertyValueSize;
typedef struct { UInt32 contents[4]; } RegEntryID;

#define true    1
#define false   0
#define noErr   0
#define kOTNoError 0
#define ENXIO   6

#define bcopy(s, d, n)  memcpy((d), (s), (n))
#define bzero(p, n)     memset((p), 0, (n))

/* --- Interrupts --- */
typedef void *InterruptSetID;
typedef int InterruptMemberNumber;
typedef struct { InterruptSetID setID; InterruptMemberNumber member; } InterruptSetMember;
typedef InterruptMemberNumber (*InterruptHandler)(InterruptSetMember, void *, UInt32);
typedef void (*InterruptEnabler)(InterruptSetID, InterruptMemberNumber);
typedef int  (*InterruptDisabler)(InterruptSetID, InterruptMemberNumber);
enum { kISTChipInterruptSource = 0, kISTPropertyMemberCount = 3 };
enum { kIsrIsComplete = -1, kIsrIsNotComplete = -2 };
#define kISTPropertyName            "driver-ist"
#define kAAPLDeviceLogicalAddress   "AAPL,address"

OSStatus RegistryPropertyGet(const RegEntryID *, const char *, void *, RegPropertyValueSize *);
OSStatus GetInterruptFunctions(InterruptSetID, InterruptMemberNumber, void **,
            InterruptHandler *, InterruptEnabler *, InterruptDisabler *);
OSStatus InstallInterruptFunctions(InterruptSetID, InterruptMemberNumber, void *,
            InterruptHandler, InterruptEnabler, InterruptDisabler);

/* --- STREAMS --- */
typedef struct msgb mblk_t;
typedef struct queue queue_t;
#include <sys/types.h>   /* dev_t */
typedef struct cred cred_t;

struct datab { UInt8 *db_base; UInt8 *db_lim; UInt8 db_type; };
struct msgb {
    mblk_t *b_next, *b_prev, *b_cont;
    UInt8 *b_rptr, *b_wptr;
    struct datab *b_datap;
};

struct module_info {
    unsigned short mi_idnum; char *mi_idname;
    long mi_minpsz, mi_maxpsz; unsigned long mi_hiwat, mi_lowat;
};
struct qinit {
    int (*qi_putp)(queue_t *, mblk_t *);
    int (*qi_srvp)(queue_t *);
    int (*qi_qopen)(queue_t *, dev_t *, int, int, cred_t *);
    int (*qi_qclose)(queue_t *, int, cred_t *);
    int (*qi_qadmin)(void);
    struct module_info *qi_minfo;
    void *qi_mstat;
};
struct streamtab { struct qinit *st_rdinit, *st_wrinit, *st_muxrinit, *st_muxwinit; };

struct queue {
    struct qinit *q_qinfo;
    mblk_t *q_first, *q_last;
    void *q_ptr;
    /* test harness only */
    int t_isWrite, t_enabled;
    queue_t *t_other;
};

typedef struct {
    struct streamtab *install_str;
    UInt32 install_flags, install_sqlvl;
    char *install_buddy;
    void *ref_load;
    UInt32 ref_count;
} install_info;

enum { M_DATA = 0, M_PROTO = 1, M_CTL = 13, M_PCSIG = 0x87 };
enum { BPRI_LO = 1, BPRI_HI = 3 };
enum { FLUSHDATA = 0 };
enum { SQLVL_MODULE = 3 };
enum { kOTModIsDriver = 1, kOTModIsPortDriver = 2, kOTModUpperIsDLPI = 4, kOTModUsesInterrupts = 8 };
#define kEnetModuleID 7101

queue_t *WR(queue_t *q);
queue_t *RD(queue_t *q);
mblk_t  *allocb(size_t size, int pri);
void    freemsg(mblk_t *mp);
int     putq(queue_t *q, mblk_t *mp);
int     putbq(queue_t *q, mblk_t *mp);
mblk_t  *getq(queue_t *q);
void    rmvq(queue_t *q, mblk_t *mp);
void    qenable(queue_t *q);
void    flushq(queue_t *q, int flag);
int     canputnext(queue_t *q);
void    putnext(queue_t *q, mblk_t *mp);
void    qreply(queue_t *q, mblk_t *mp);

mblk_t  *mi_timer_alloc(queue_t *q, size_t size);
void    mi_timer(mblk_t *mp, unsigned long ms);
Boolean mi_timer_valid(mblk_t *mp);
void    mi_timer_free(mblk_t *mp);

void    *OTAllocMem(size_t n);
void    OTFreeMem(void *p);
void    OTEnterInterrupt(void);
void    OTLeaveInterrupt(void);

/* --- DLPI template (shape only) --- */
typedef struct dle_addr_s { struct dle_addr_s *dlea_next; UInt8 dlea_addr[6]; } dle_addr_t;
typedef struct {
    void (*dlehw_start)(void *);
    void (*dlehw_stop)(void *);
    void (*dlehw_address_filter_reset)(void *, dle_addr_t *, UInt32, UInt32, UInt32, UInt32, UInt32);
    int  (*dlehw_send_error)(void *, mblk_t *, UInt32);
    unsigned long dlehw_recv_error_flags;
} dlehw_t;
typedef struct {
    unsigned long bytes_received, unicast_frames_received, bytes_sent, unicast_frames_sent,
        multicast_frames_received, broadcast_frames_received, receive_discards, receive_errors,
        receive_unknown_protos, multicast_frames_sent, broadcast_frames_sent,
        transmit_discards, transmit_errors, mtu, speed;
} dle_interface_status_t;
typedef struct {
    unsigned long ether_chip_set, receive_alignment_errors, receive_fcs_errors,
        receive_frames_too_long, receive_internal_mac_errors,
        transmit_single_collision_frames, transmit_multiple_collision_frames,
        transmit_sqe_test_errors, transmit_deferred_transmissions, transmit_late_collisions,
        transmit_excessive_collisions, transmit_internal_mac_errors,
        transmit_carrier_sense_errors;
} dle_ethernet_status_t;
typedef struct {
    dlehw_t dle_hw;
    UInt8 dle_factory_addr[6], dle_current_addr[6];
    dle_interface_status_t dle_istatus;
    dle_ethernet_status_t dle_estatus;
    UInt32 dle_refcnt;
} dle_t;
typedef struct { UInt32 dcl_state; void *dcl_hw; queue_t *dcl_rq; UInt32 pad[8]; } dcl_t;

void    dle_init(dle_t *dle, size_t xtra);
void    dle_terminate(dle_t *dle);
int     dle_open(dle_t *dle, queue_t *q, dev_t *devp, int flag, int sflag, cred_t *credp, size_t dcl_len);
int     dle_close(queue_t *q);
void    dle_inbound(dle_t *dle, mblk_t *mp);
void    dle_rsrv_ctl(queue_t *q, mblk_t *mp);
mblk_t  *dle_wput(queue_t *q, mblk_t *mp);

#endif
