; LZ8S ultra-simple LZ based compressor
; -------------------------------------
;
; (c) 2025 DMSC
; Code under MIT license, see LICENSE file.
;
; Program to decompress data compressed with lz8s -x

dst = $80
src = $82
tmp = $84
setx= $86
cnt = $87

        org $600

        lda 88
        sta dst
        lda 89
        sta dst+1
        lda #<(input_data)
        sta src
        lda #>(input_data)
        sta src+1
        ; Number of compressed blocks
        lda #18
        sta cnt

; src: pointer to source data
; dst: pointer to destination data
; tmp: temporary
get_literal:
        dec cnt
        beq do_end
        jsr get_count
        tay
        beq get_match
        jsr put_byte
get_match:
        jsr get_count
        tay
        beq get_literal
        jsr get_byte
        clc
;        eor #$FF       ; This is needed for lz8s without '-x'
        adc dst
        sta tmp
        lda dst+1
        adc #$FF
        sta tmp+1
        ldx #2
        jsr put_byte
        beq get_literal

get_count:
        ldx #0

get_byte:
        lda (src,x)
        inc src,x
        bne @+
        inc src+1,x
@:
do_end: rts

put_byte:
        stx setx
ploop:  ldx setx
        jsr get_byte
        ldx #0
        sta (dst,x)
        inc dst
        bne @+
        inc dst+1
@       dey
        bne ploop
        rts

input_data:
        .byte 1,14,138,255,1,138,38,116,3,212,239,212,39,217,0,36
        .byte 215,1,239,6,255,0,73,136,0,38,176,0,7,138,0,31
        .byte 134,1,139,14,255,0,67,16,0,37,136,1,239,16,255,0
        .byte 37,219,0,36,50,0,32,97,0,147,216
end_data:
