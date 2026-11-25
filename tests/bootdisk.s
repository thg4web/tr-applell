; bootdisk.s: a 3-block test disk for the block card and the ROM's boot path.
; Block 0 boots at $0801 (X = slot x 16), prints a line, reads block 1 through the
; ProDOS driver ($Cn00 + $CnFF) and block 2 through the SmartPort entry (driver + 3),
; prints the text found in each, then BRKs into the Monitor.
        .setcpu "6502"
COUT    = $FDED
CROUT   = $FD8E
PTR     = $06           ; $Cn00
DRV     = $08           ; ProDOS driver entry
SPV     = $EB           ; SmartPort entry
STR     = $FA           ; string pointer

        .segment "DISK"
        .org $0800
        .byte $01
start:  txa                     ; X = slot x 16 -> $Cn
        lsr a
        lsr a
        lsr a
        lsr a
        ora #$C0
        sta PTR+1
        sta DRV+1
        sta SPV+1
        lda #0
        sta PTR
        ldy #$FF
        lda (PTR),y             ; $CnFF: low byte of the driver
        sta DRV
        clc
        adc #3
        sta SPV
        lda #<msg0
        ldy #>msg0
        jsr print
        lda #1                  ; ProDOS block read: block 1 -> $1000
        sta $42
        stx $43
        lda #$00
        sta $44
        sta $47
        lda #$10
        sta $45
        lda #1
        sta $46
        jsr calldrv
        bcs fail
        lda #$00
        ldy #$10
        jsr print
        jsr callsp              ; SmartPort READ BLOCK: block 2 -> $1200
        .byte $01
        .word plist
        bcs fail
        lda #$00
        ldy #$12
        jsr print
        brk
        .byte 0
fail:   lda #<msgf
        ldy #>msgf
        jsr print
        brk
        .byte 0
calldrv: jmp (DRV)
callsp: jmp (SPV)
print:  sta STR
        sty STR+1
        ldy #0
@loop:  lda (STR),y
        beq @done
        jsr COUT
        iny
        bne @loop
@done:  jmp CROUT
plist:  .byte 3, 1
        .word $1200
        .byte 2, 0, 0

.macro hstr s
        .repeat .strlen(s), i
        .byte .strat(s, i) | $80
        .endrepeat
        .byte 0
.endmacro
msg0:   hstr "BOOT OK FROM BLOCK 0"
msgf:   hstr "DISK READ FAILED"

        .res $0A00 - *, $00     ; block 1
        hstr "BLOCK 1 VIA PRODOS DRIVER"
        .res $0C00 - *, $00     ; block 2
        hstr "BLOCK 2 VIA SMARTPORT"
        .res $0E00 - *, $00
