# LZ8S ultra-simple LZ based compressor

This is a compressor for a byte aligned, simple LZ based compressor. It is
intended as a compressor for small data that needs a very fast and short
decompression on 8-bit machines.

The compressor uses a near optimal parsing to produce optimal compression for
small and compressible files without being exponentially slow for large files.

## Compression Format

The compression is based on a simple LZ scheme, all data is compressed as
blocks of "literal data", bytes that are copied from the input to the output
unchanged, and "matched data", bytes that are copied from the already
decompressed data to the output.

The compressed data always start with a literal block, then a match block and
literal again, until all the data is consumed.

This is a simple pseudocode of the decompression:

```
    DO
        // Literal
        GetCount N
        FOR i = 1 TO N
            output = input

        // Match
        GetCount N
        IF N > 0
            GetOffset O
            FOR i = 1 TO N
                output = output - O
    LOOP
```

### Storing Offsets

The standard compressor stores the offsets as an 8 bit number, with 0 meaning
the last decompressed byte (current position minus one) and 255 meaning the
positions just 256 bytes prior to the current one.

If the match count is 0, the offset is skipped. This is used to split long
literals, by including a match of zero length between to shorter literals.

There are two variations on the scheme:

* The `-o` option allows to set the number of bits of the offset, the default is
  8 bits.

  Selecting 0 as the number of bits disables the offsets, so matches are
  always for the last output byte, transforming the compressor to a simple RLE
  (run length encoded).

  Selecting between 1 and 8 bits, the offsets are stored as a full byte.
  Selecting between 9 and 16 bits stores writes two bytes, the first is the
  low part and the second byte is the high part of the offset.

  As the compressor always stores full bytes, the optimal choices are 0, 8 or
  16 bits, a number of bits between those are only useful to limit the buffer
  size on a streaming decompresor.

* The `-n` options makes the compressor write the offset even when the count is
  zero, disabling this optimization. This could make the decompression code
  smaller, and most of the times the compressor is able to find a combination
  of matches that makes the compressed file the same length or only a couple of
  bytes larger.

* The `-A` option changes the way the offsets are stored - instead of storing
  the position relative to the current output, it is written as the position in
  the decompression buffer - this could make the decompression code simpler.

  The option needs a parameter, te "address" of the buffer - this tells the
  compressor that the first byte of the output will be stored in this address
  (relative to the start of the buffer) and so on.

  To use this option, the number of bits of the offset should be 8 or 16, any
  other number is invalid.


### Storing Counts

The count is stored as one byte, for counts from 0 to 255 inclusive. A count
of zero basically means to skip this literal or match.

You can also change the limit of the counts with two options:

* Option `-l` sets the limit for the literal lengths, from 1 to 32895.

* Option `-m` sets the limit for the math lengths, from 1 to 32895.

When the limit is more than 255, a different scheme is used to store the count:
* if the count is less than 128, it is stored as one byte directly;
* if not, the count is the first byte plus the second byte times 128.

## Sample decompression code

Sample code in a few languages

### Turbo Basic XL

```
1000 SRC=ADR("COMPRESSED DATA STRING")
1010 LST=SRC+DATA_LEN : X=LST
1020 PTR=DPEEK(88) : REM Output to Screen
1030 WHILE SRC<LST
1040   CNT=PEEK(SRC) : SRC=SRC+1
1050   IF X<SRC
1060      X=SRC : SRC=SRC+CNT
1070   ELSE
1080      X=PTR-PEEK(SRC)-1 : SRC=SRC+1
1090   ENDIF
1100   MOVE X, PTR, CNT
1110   PTR=PTR+CNT
1120 WEND
```

### Assembler

```
; src: pointer to source data
; ptr: pointer to destination data
; tmp: temporary
get_literal:
        jsr read_src
        tax
        beq get_match
loop1:  lda (src),y
        sta (ptr),y
        iny
        dex
        bne loop1
        tya
        clc
        adc src
        sta src
        bcc skip0
        inc src+1
skip0:  jsr adjust_ptr
get_match:
        jsr read_src
        tax
        beq get_literal
        jsr read_src
        eor #$FF
        clc
        adc ptr
        sta tmp
        lda ptr+1
        adc #$FF
        sta tmp+1
loop2:  lda (tmp),y
        sta (ptr),y
        iny
        dex
        bne loop2
        jsr adjust_ptr
        jmp get_literal
read_src:
        lda src
        cmp #<end_data
        bne nend
        lda src+1
        cmp #>end_data
        beq doend
nend:   ldy #0
        lda (src),y
        inc src
        bne skip1
        inc src+1
skip1:  rts
adjust_ptr:
        tya
        clc
        adc ptr
        sta ptr
        bcc skip2
        inc ptr+1
skip2:  rts
doend:  pla
        pla
        rts
```
