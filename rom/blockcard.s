; blockcard.s: firmware for our virtual 3.5-inch block-device card, slot 5 ($C500-$C5FF).
; Our own design on documented conventions:
;   [PTR] ProDOS 8 Technical Reference Manual, block-device firmware: ID bytes
;         $Cn01=$20, $Cn03=$00, $Cn05=$03; $Cn07=$00 means a SmartPort is present;
;         $CnFC/$CnFD total blocks, $CnFE status byte, $CnFF low byte of the driver;
;         the SmartPort entry is the driver + 3; a boot loads block 0 at $0800 and
;         jumps to $0801 with X = slot x 16; driver call uses zero page $42-$47.
;   [SP]  SmartPort interface (Apple IIc / IIGS firmware references): JSR entry,
;         then a command byte and a parameter-list address inline.
; The two driver entries are trap opcodes ($12, $22): the emulator does the transfer
; and returns to the caller (see emu/a2.c). The total-block count is filled in by
; the emulator when a disk is attached.

        .setcpu "6502"
SLOT    = 5
UNIT    = SLOT << 4
CMD     = $42
UNITNUM = $43
BUFL    = $44
BUFH    = $45
BLKL    = $46
BLKH    = $47
MON     = $FF65

        .segment "CARD"
        .org $C500 + 0
; ID bytes sit in the operands of harmless loads [PTR].
        ldx #$20                ; $C501 = $20
        ldy #$00                ; $C503 = $00
        ldx #$03                ; $C505 = $03
        ldx #$00                ; $C507 = $00: SmartPort present
; Boot [PTR]: block 0 to $0800, then JMP $0801 with X = slot x 16.
boot:   lda #$01                ; read
        sta CMD
        lda #UNIT
        sta UNITNUM
        lda #$00
        sta BUFL
        sta BLKL
        sta BLKH
        lda #$08
        sta BUFH
        jsr driver
        bcs @fail
        ldx #UNIT
        jmp $0801
@fail:  jmp MON

        .res $C540 - *, $00
driver: .byte $12               ; trap: ProDOS block call
        .byte $EA, $EA
        .assert * = driver + 3, error, "SmartPort entry must be driver + 3"
smartport:
        .byte $22               ; trap: SmartPort call

        .res $C5FC - *, $00
        .word 0                 ; $C5FC/$C5FD: total blocks (set by the emulator)
        .byte $87               ; $C5FE: removable, write, read, status [PTR]
        .byte <driver           ; $C5FF
