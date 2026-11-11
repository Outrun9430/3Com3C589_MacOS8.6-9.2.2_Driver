/*
 * EL3Reg.h - 3Com EtherLink III (3C589 family) register definitions.
 *
 * Register layout and command semantics follow the 3Com EtherLink III
 * parallel-tasking design shared by the 3C509 (ISA) and 3C589 (PC Card);
 * the Linux 3c589_cs and BSD "ep" drivers were used as references for
 * the PC Card specific behaviour.
 */
#ifndef EL3REG_H
#define EL3REG_H

/* Offsets from the I/O base that are valid in every register window. */
#define EL3_CMD             0x0E    /* write: command register          */
#define EL3_STATUS          0x0E    /* read:  status register           */

/* Commands: top five bits are the opcode, low eleven the argument. */
#define CMD_TotalReset      (0u << 11)
#define CMD_SelectWindow    (1u << 11)
#define CMD_StartCoax       (2u << 11)
#define CMD_RxDisable       (3u << 11)
#define CMD_RxEnable        (4u << 11)
#define CMD_RxReset         (5u << 11)
#define CMD_RxDiscard       (8u << 11)
#define CMD_TxEnable        (9u << 11)
#define CMD_TxDisable       (10u << 11)
#define CMD_TxReset         (11u << 11)
#define CMD_AckIntr         (13u << 11)
#define CMD_SetIntrEnb      (14u << 11)
#define CMD_SetStatusEnb    (15u << 11)
#define CMD_SetRxFilter     (16u << 11)
#define CMD_SetTxThreshold  (18u << 11)
#define CMD_StatsEnable     (21u << 11)
#define CMD_StatsDisable    (22u << 11)
#define CMD_StopCoax        (23u << 11)

/* Status / interrupt bits. */
#define ST_IntLatch         0x0001
#define ST_AdapterFailure   0x0002
#define ST_TxComplete       0x0004
#define ST_TxAvailable      0x0008
#define ST_RxComplete       0x0010
#define ST_RxEarly          0x0020
#define ST_IntReq           0x0040
#define ST_StatsFull        0x0080
#define ST_CmdBusy          0x1000
#define ST_WindowMask       0xE000  /* current window number            */
#define ST_Window1          0x2000

/* SetRxFilter argument bits. */
#define RXF_Station         0x01
#define RXF_Multicast       0x02
#define RXF_Broadcast       0x04
#define RXF_Promiscuous     0x08

/* Window 0: configuration / EEPROM. */
#define W0_CONFIG_CTRL      0x04    /* bit 0: activate adapter          */
#define W0_ADDR_CONFIG      0x06    /* bits 14-15: transceiver select   */
#define W0_RESOURCE_CONFIG  0x08    /* IRQ select; 0x3F00 on PC Card    */
#define W0_EEPROM_CMD       0x0A
#define W0_EEPROM_DATA      0x0C
#define EEPROM_READ         0x0080
#define EEPROM_BUSY         0x8000
#define XCVR_TP             (0u << 14)
#define XCVR_AUI            (1u << 14)
#define XCVR_COAX           (3u << 14)

/* Window 1: operating set. */
#define W1_TX_FIFO          0x00
#define W1_RX_FIFO          0x00
#define W1_RX_STATUS        0x08
#define W1_TX_STATUS        0x0B    /* byte register                    */
#define W1_TX_FREE          0x0C
#define RXS_Incomplete      0x8000
#define RXS_Error           0x4000
#define RXS_ErrMask         0x3800
#define RXS_LenMask         0x07FF
#define TXS_Complete        0x80
#define TXS_Jabber          0x20
#define TXS_Underrun        0x10
#define TXS_MaxCollisions   0x08
#define TXS_StatusOverflow  0x04

/* Window 2: station address, bytes 0-5. */

/* Window 4: diagnostics / media. */
#define W4_FIFO_DIAG        0x04
#define W4_MEDIA            0x0A
#define FIFOD_TxOverrun     0x0400
#define FIFOD_RxUnderrun    0x2000
#define MEDIA_LED           0x0001  /* link LED enable (3C589E)         */
#define MEDIA_TP            0x00C0  /* link beat + jabber enable        */
#define MEDIA_LinkBeat      0x0800  /* link beat detected               */

/* Window 6: statistics (reading clears). */
#define W6_CARRIER_LOST     0x00
#define W6_SQE_ERRORS       0x01
#define W6_MULTI_COLL       0x02
#define W6_SINGLE_COLL      0x03
#define W6_LATE_COLL        0x04
#define W6_RX_OVERRUN       0x05
#define W6_TX_FRAMES_OK     0x06
#define W6_RX_FRAMES_OK     0x07
#define W6_TX_DEFERRALS     0x08
#define W6_RX_BYTES_OK      0x0A
#define W6_TX_BYTES_OK      0x0C

#endif /* EL3REG_H */
