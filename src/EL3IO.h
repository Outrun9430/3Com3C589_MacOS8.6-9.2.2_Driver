/*
 * EL3IO.h - hardware access primitives for the EtherLink III PC Card.
 *
 * Under PC Card Manager 3.0 the card's 16 I/O ports are mapped into the
 * PowerBook's address space; the logical base address is published in
 * the device node's "AAPL,address" property.  The ports are therefore
 * read and written with ordinary loads and stores:
 *
 *   - Registers are 16-bit little-endian quantities, so they are
 *     byte-swapped on the (big-endian) PowerPC.
 *   - FIFO data is a byte stream.  A 16-bit store puts the byte at the
 *     lower address on data lines D0-D7, which is where the card expects
 *     the first byte of the stream, so packet data is moved with plain
 *     (unswapped) 16-bit accesses.
 *
 * Every access is followed by an eieio so that the processor cannot
 * reorder or merge accesses to the device.
 *
 * When EL3_HOST_TEST is defined these primitives are supplied by the
 * register-level simulator in test/ instead.
 */
#ifndef EL3IO_H
#define EL3IO_H

#ifdef EL3_HOST_TEST

extern UInt16 el3_rd16(void *io, int off);
extern void   el3_wr16(void *io, int off, UInt16 v);
extern UInt8  el3_rd8(void *io, int off);
extern void   el3_wr8(void *io, int off, UInt8 v);
extern void   el3_fifo_write(void *io, const UInt8 *p, UInt32 words);
extern void   el3_fifo_read(void *io, UInt8 *p, UInt32 words);

#else /* real hardware */

#if defined(__MWERKS__) || defined(__MRC__)
#define EL3_EIEIO()     __eieio()
#elif defined(__GNUC__)
#define EL3_EIEIO()     __asm__ volatile ("eieio")
#else
#define EL3_EIEIO()     SynchronizeIO()     /* DriverServices */
#endif

#define EL3_REG16(io, off)  (*(volatile UInt16 *)((UInt8 *)(io) + (off)))
#define EL3_REG8(io, off)   (*(volatile UInt8  *)((UInt8 *)(io) + (off)))

static UInt16 el3_rd16(void *io, int off)
{
    UInt16 v = EL3_REG16(io, off);
    EL3_EIEIO();
    return (UInt16)((v << 8) | (v >> 8));
}

static void el3_wr16(void *io, int off, UInt16 v)
{
    EL3_REG16(io, off) = (UInt16)((v << 8) | (v >> 8));
    EL3_EIEIO();
}

static UInt8 el3_rd8(void *io, int off)
{
    UInt8 v = EL3_REG8(io, off);
    EL3_EIEIO();
    return v;
}

static void el3_wr8(void *io, int off, UInt8 v)
{
    EL3_REG8(io, off) = v;
    EL3_EIEIO();
}

/* Move 'words' 16-bit units between a word-aligned buffer and the FIFO. */
static void el3_fifo_write(void *io, const UInt8 *p, UInt32 words)
{
    const UInt16 *src = (const UInt16 *)p;

    while (words--) {
        EL3_REG16(io, 0) = *src++;
        EL3_EIEIO();
    }
}

static void el3_fifo_read(void *io, UInt8 *p, UInt32 words)
{
    UInt16 *dst = (UInt16 *)p;

    while (words--) {
        *dst++ = EL3_REG16(io, 0);
        EL3_EIEIO();
    }
}

#endif /* EL3_HOST_TEST */
#endif /* EL3IO_H */
