; monitor.s: TR ][ Monitor, version 0.3, for a 64K Apple ][ Plus.
; 0.1 (2026-11-24): Monitor, text I/O, "*" prompt.
; 0.2: boots a disk card; IORTS.
; 0.3: fences. Every documented entry address in $F800-$FFFF now holds either our
;      routine (or a JMP to it) or a $02 trap byte, never the middle of other code.
;      (0.2 had bodies there; Oregon Trail's loader called MOVE at $FE2C and landed
;      inside the BRK handler.) Bodies moved to an extension ROM at $C800-$CFFF.
;      Added MOVE, NXTA4, NXTA1.
;
; Built from documented behavior, not from any listing or dump of Apple's ROM. Honest
; limit, recorded on 2026-11-24: its authors have very likely been exposed to the
; original Monitor listing, so this is a documented-behavior reimplementation, not a
; strict clean room.
;
; Provenance key, used on every routine:
;   [RM]  Apple II Reference Manual (1979): entry point addresses, zero-page and page-3
;         map, Monitor command syntax, routine contracts (what goes in, what comes out).
;   [TN7] Apple II Miscellaneous Technical Note #7: machine ID bytes.
;   [PTR] ProDOS 8 Technical Reference Manual: disk-card ID bytes for the boot scan.
;   [HW]  Apple II Reference Manual hardware chapters: soft switches, keyboard, speaker,
;         text-page layout.
;   [OURS] our own design, no outside contract.
; Where documented addresses sit only a few bytes apart, the layout forces the code
; (for example COUT is exactly a 3-byte JMP (CSWL)); those spots say "forced".
;
; Layout [OURS]:
;   $F800-$FFFF  entry stubs at documented addresses, fences ($02) at documented
;                addresses we have not written yet, $02 everywhere else.
;   $C800-$CFFF  extension ROM with the routine bodies. A stock ][ Plus has no
;                motherboard ROM here (the space belongs to peripheral cards); our
;                emulated disk card does not use it. A deliberate departure.
; The emulator stops on $02 in ROM and logs it: that log is the to-do list.
; Fence addresses come from published entry-point tables; a wrong one costs nothing
; but a byte of packing. Not yet: lo-res graphics, the disassembler (L), the M and V
; commands, Esc editing, tape, the mini-assembler, BASIC.

        .setcpu "6502"

; ---- zero page, page 2 and page 3 [RM] ----
LOC0    = $00           ; slot-scan pointer during boot
LOC1    = $01
WNDLFT  = $20
WNDWDTH = $21
WNDTOP  = $22
WNDBTM  = $23
CH      = $24
CV      = $25
BASL    = $28
BASH    = $29
BAS2L   = $2A
BAS2H   = $2B
NUMDIG  = $2F           ; [OURS] digits seen by getnum (a Monitor-owned scratch byte)
MODE    = $31
INVFLG  = $32
PROMPT  = $33
YSAV    = $34
YSAV1   = $35
CSWL    = $36
CSWH    = $37
KSWL    = $38
KSWH    = $39
PCL     = $3A
PCH     = $3B
A1L     = $3C
A1H     = $3D
A2L     = $3E
A2H     = $3F
A3L     = $40
A3H     = $41
A4L     = $42
A4H     = $43
ACC     = $45           ; register save area: A, X, Y, P, S
XREG    = $46
YREG    = $47
STATUS  = $48
SPNT    = $49
RNDL    = $4E
RNDH    = $4F
IN      = $0200         ; input buffer
BRKV    = $03F0         ; BRK handler vector
SOFTEV  = $03F2         ; warm-start (Reset) vector
PWREDUP = $03F4         ; = high byte of SOFTEV EOR $A5 once set up
USRADR  = $03F8         ; Ctrl-Y jumps here
NMIV    = $03FB
IRQV    = $03FE

; ---- hardware [HW] ----
KBD     = $C000
KBDSTRB = $C010
SPKR    = $C030
TXTSET  = $C051
MIXCLR  = $C052
LOWSCR  = $C054
PADDL0  = $C064
PTRIG   = $C070

; Pad with $02 up to a documented entry point; refuse to assemble on an overlap.
.macro at addr
        .assert * <= addr, error, .sprintf("code runs into the fixed entry point $%04X", addr)
        .if addr > *
        .res addr - *, $02
        .endif
.endmacro

; A documented entry point we have not written: it must hold the $02 trap byte.
.macro fence addr
        at addr
        .byte $02
.endmacro

; High-bit ASCII text, the way the Apple screen wants it.
.macro hstr s
        .repeat .strlen(s), i
        .byte .strat(s, i) | $80
        .endrepeat
.endmacro

; ======================================================================
; Extension ROM, $C800-$CFFF: routine bodies
; ======================================================================
        .segment "EXT"
        .org $C800

; COUT1 [RM]: show the character in A at CH/CV inside the window, wrap and
; scroll as needed; Return, line feed, backspace and bell act. A, X, Y kept.
; Stored as A AND INVFLG [RM]. Characters below $A0 that are not one of the
; four controls are ignored [OURS].
cout1_body:
        pha
        sty YSAV1
        cmp #$A0
        bcc @control
        and INVFLG
        ldy CH
        sta (BASL),y
        iny
        cpy WNDWDTH
        bcc @keep
        jsr newline
        jmp @done
@keep:  sty CH
@done:  ldy YSAV1
        pla
        rts
@control:
        cmp #$8D
        bne @notcr
        jsr newline
        jmp @done
@notcr: cmp #$8A
        bne @notlf
        jsr linefeed
        jmp @done
@notlf: cmp #$88
        bne @notbs
        jsr backspace
        jmp @done
@notbs: cmp #$87
        bne @done
        jsr BELL1
        jmp @done

; [OURS] helpers for COUT1. Clobber A and Y; keep X.
newline:
        lda #0
        sta CH
linefeed:
        inc CV
        lda CV
        cmp WNDBTM
        bcc @ok
        dec CV
        jmp scroll_body
@ok:    jmp vtab_body
backspace:
        dec CH
        bpl @ok
        lda WNDWDTH             ; from column 0: end of the line above
        sta CH
        dec CH
        lda CV
        cmp WNDTOP
        beq @top
        dec CV
        jmp vtab_body
@top:   lda #0
        sta CH
@ok:    rts

; VTAB / VTABZ [RM]: point BASL/BASH at row CV (VTABZ: row in A), plus WNDLFT.
vtab_body:
        lda CV
vtabz_body:
        jsr bascalc_body
        clc
        lda WNDLFT
        adc BASL
        sta BASL
        rts

; BASCALC [RM]: row in A (0-23) -> BASL/BASH on text page 1.
; Layout [HW]: $400 + (row mod 8) x $80 + (row div 8) x $28. Arithmetic [OURS].
bascalc_body:
        pha
        lsr a
        and #$03
        ora #$04
        sta BASH                ; $04 + (row mod 8) div 2
        pla
        pha
        and #$18                ; (row div 8) x 8
        sta BASL
        asl a
        asl a
        adc BASL                ; x 5 = (row div 8) x $28; carry is clear
        sta BASL
        pla
        lsr a
        bcc @even
        lda BASL
        ora #$80                ; odd row: + $80
        sta BASL
@even:  rts

; SCROLL [RM]: move the window up one line, blank the bottom line.
scroll_body:
        lda WNDTOP
@row:   pha                     ; destination row
        jsr vtabz_body
        lda BASL
        sta BAS2L
        lda BASH
        sta BAS2H
        pla
        clc
        adc #1                  ; source row
        cmp WNDBTM
        bcs @last
        pha
        jsr vtabz_body
        ldy WNDWDTH
        dey
@copy:  lda (BASL),y
        sta (BAS2L),y
        dey
        bpl @copy
        pla
        jmp @row
@last:  ldy WNDWDTH
        dey
        lda #$A0
@clr:   sta (BAS2L),y
        dey
        bpl @clr
        jmp vtab_body

; CLEOLZ [RM]: blank from column Y to the end of the window line.
cleolz_body:
        lda #$A0
@loop:  cpy WNDWDTH
        bcs @done
        sta (BASL),y
        iny
        bne @loop
@done:  rts

; CLREOP [RM]: blank from the cursor to the end of the window.
clreop_body:
        ldy CH
        lda CV
@row:   pha
        jsr vtabz_body
        jsr cleolz_body
        ldy #0
        pla
        clc
        adc #1
        cmp WNDBTM
        bcc @row
        jmp vtab_body

; HOME [RM]: cursor to the window's top left, blank the window.
home_body:
        lda WNDTOP
        sta CV
        lda #0
        sta CH
        jmp clreop_body

; RDKEY [RM]: show a flashing cursor, then go through KSW. The input routine
; gets the covered character in A and Y = CH [RM].
rdkey_body:
        ldy CH
        lda (BASL),y
        pha
        and #$3F
        ora #$40                ; flashing copy
        sta (BASL),y
        pla
        jmp (KSWL)

; KEYIN [RM]: wait for a key, counting RNDL/RNDH while waiting [RM]; put the
; covered character back; return the key with its high bit set.
keyin_body:
        pha
@wait:  inc RNDL
        bne @noc
        inc RNDH
@noc:   bit KBD
        bpl @wait
        pla
        sta (BASL),y
        lda KBD
        bit KBDSTRB
        rts

; GETLN1 [RM]: read a line into IN without a prompt. Left arrow backs up,
; Ctrl-X cancels (prints a backslash, starts over), right arrow copies the
; character under the cursor [RM]. Returns X = length; IN ends with $8D [RM].
getln1_body:
        ldx #0
@next:  jsr RDCHAR
        cmp #$88
        beq @back
        cmp #$98
        beq @cancel
        cmp #$95
        bne @store
        ldy CH                  ; right arrow: screen glyph back to a character
        lda (BASL),y
        and #$3F
        cmp #$20
        bcs @hi
        ora #$40
@hi:    ora #$80
@store: sta IN,x
        cmp #$8D
        beq @done
        jsr COUT
        inx
        bne @next
@cancel:
        lda #$DC                ; backslash
        jsr COUT
        jmp GETLNZ
@back:  txa
        beq @next
        dex
        lda #$88
        jsr COUT
        jmp @next
@done:  jmp CROUT1

; PRBYTE / PRHEX [RM]: A as two hex digits / low nibble as one. A is lost.
prbyte_body:
        pha
        lsr a
        lsr a
        lsr a
        lsr a
        jsr prhex_body
        pla
prhex_body:
        and #$0F
        ora #$B0
        cmp #$BA
        bcc @out
        adc #$06                ; carry is set: +7 turns $BA-$BF into 'A'-'F'
@out:   jmp COUT

prbl2_body:
        lda #$A0
        jsr COUT
        dex
        bne prbl2_body
        rts

; BELL1 [RM]: a tone of about 1 kHz for about 0.1 s on the speaker [HW].
bell1_body:
        ldy #$C0
@loop:  lda #$0C
        jsr WAIT
        lda SPKR
        dey
        bne @loop
        rts

; INIT / SETTXT [RM]: text mode, page 1, full-screen window, cursor at the
; bottom left.
init_body:
        lda #0
        sta WNDLFT
        sta WNDTOP
        lda #40
        sta WNDWDTH
        lda #24
        sta WNDBTM
        lda TXTSET
        lda MIXCLR
        lda LOWSCR
        lda #23
        sta CV
        lda #0
        sta CH
        jmp vtab_body

; IOSAVE / IOREST [RM]: save A, X, Y, P (and S) at $45-$49 / restore them.
iosave_body:
        sta ACC
        stx XREG
        sty YREG
        php
        pla
        sta STATUS
        tsx
        stx SPNT
        cld
        rts
iorest_body:
        lda STATUS
        pha
        lda ACC
        ldx XREG
        ldy YREG
        plp
        rts

; INPORT / OUTPORT [RM]: A = slot number; 0 means keyboard / screen.
; A slot n routes through $Cn00 [RM].
inport_body:
        and #$07
        beq @kbd
        ora #$C0
        sta KSWH
        lda #0
        sta KSWL
        rts
@kbd:   lda #<KEYIN
        sta KSWL
        lda #>KEYIN
        sta KSWH
        rts
outport_body:
        and #$07
        beq @scr
        ora #$C0
        sta CSWH
        lda #0
        sta CSWL
        rts
@scr:   lda #<COUT1
        sta CSWL
        lda #>COUT1
        sta CSWH
        rts

; PREAD [RM]: X = paddle 0-3 -> Y = 0-255, one count every 11 cycles (4+2+2+3 below), stopping
; at 255. The emulator times each paddle so this returns exactly the value set (emu/a2.c).
pread_rest:
        ldy #0
@loop:  lda PADDL0,x
        bpl @done
        iny
        bne @loop
        dey
@done:  rts

; IRQ/BRK [RM]: A saved in ACC; BRK goes through BRKV ($3F0), other IRQs
; through IRQV ($3FE).
irq_body:
        sta ACC
        pla
        pha
        asl a
        asl a
        asl a                   ; B flag to bit 7
        bmi @brk
        lda ACC
        jmp (IRQV)
@brk:   jmp (BRKV)

; RESET [RM]: set up text and I/O hooks; warm start through SOFTEV when the
; power-up byte matches, else cold start: set the vectors, clear the screen,
; banner, then boot a disk card or enter the Monitor. (No BASIC yet.)
reset_body:
        cld
        jsr SETNORM
        jsr INIT
        jsr SETVID
        jsr SETKBD
        lda SOFTEV+1
        eor #$A5
        cmp PWREDUP
        bne @cold
        jmp (SOFTEV)
@cold:  lda #<MON
        sta SOFTEV
        lda #>MON
        sta SOFTEV+1
        eor #$A5
        sta PWREDUP
        lda #<brk_body
        sta BRKV
        lda #>brk_body
        sta BRKV+1
        lda #$4C                ; Ctrl-Y: JMP MON until a program changes it
        sta USRADR
        lda #<MON
        sta USRADR+1
        lda #>MON
        sta USRADR+2
        jsr HOME
        ldy #0
@ban:   lda banner,y
        beq @go
        jsr COUT
        iny
        bne @ban
; Boot [RM]: the highest-numbered slot with a disk card, found by its ID bytes
; $Cn01/$Cn03/$Cn05 = $20/$00/$03 [PTR]; the pointer lives in $00/$01 [RM].
; No card: the Monitor.
@go:    lda #0
        sta LOC0
        ldx #$C7
@slot:  stx LOC1
        ldy #$01
        lda (LOC0),y
        cmp #$20
        bne @next
        ldy #$03
        lda (LOC0),y
        bne @next
        ldy #$05
        lda (LOC0),y
        cmp #$03
        bne @next
        jmp (LOC0)
@next:  dex
        cpx #$C0
        bne @slot
        jmp MON
banner: hstr "TR ][ MONITOR 0.3"
        .byte $8D, $00

; [OURS] hex digits at IN,Y -> A2; NUMDIG = digit count; Y stops on the
; first other character, which is left in A.
getnum:
        lda #0
        sta A2L
        sta A2H
        sta NUMDIG
@next:  lda IN,y
        cmp #$B0
        bcc @end
        cmp #$BA
        bcc @dec
        cmp #$C1
        bcc @end
        cmp #$C7
        bcs @end
        sbc #$B6                ; carry is clear: 'A'-'F' become $0A-$0F
        jmp @add
@dec:   and #$0F
@add:   ldx #4
@shift: asl A2L
        rol A2H
        dex
        bne @shift
        ora A2L
        sta A2L
        inc NUMDIG
        iny
        bne @next
@end:   rts

; BRK handler [RM]: show where the BRK was and the registers, then the Monitor.
; Format [OURS]: "0305- A=C1 X=00 Y=00 P=30 S=F7".
brk_body:
        stx XREG
        sty YREG
        pla
        sta STATUS
        pla
        sta PCL
        pla
        sta PCH
        tsx
        stx SPNT
        cld
        sec
        lda PCL
        sbc #2                  ; the BRK itself, not BRK + 2
        sta PCL
        bcs @nb
        dec PCH
@nb:    jsr CROUT
        lda PCH
        jsr PRBYTE
        lda PCL
        jsr PRBYTE
        lda #$AD                ; '-'
        jsr COUT
        ldx #0
@reg:   lda #$A0
        jsr COUT
        lda regname,x
        jsr COUT
        lda #$BD                ; '='
        jsr COUT
        lda ACC,x
        jsr PRBYTE
        inx
        cpx #5
        bcc @reg
        jmp MON
regname: hstr "AXYPS"

; [OURS] act on a number just typed, according to MODE:
;   0 nothing pending, 1 an address waiting to be shown, '.' a range end,
;   ':' a byte to store at A3.
gotnum:
        lda MODE
        cmp #$AE
        beq @range
        cmp #$BA
        beq @store
        cmp #$01
        bne @start
        lda A2L                 ; two addresses in a row: show the first too [RM],
        pha                     ; keeping the new number (examine1 uses A2)
        lda A2H
        pha
        jsr examine1
        pla
        sta A2H
        pla
        sta A2L
@start: lda A2L
        sta A1L
        lda A2H
        sta A1H
        lda #$01
        sta MODE
        rts
@range: jsr dump
        lda #0
        sta MODE
        rts
@store: ldy #0
        lda A2L
        sta (A3L),y
        inc A3L
        bne @s1
        inc A3H
@s1:    rts

; Memory display [RM]: "0300- A9 C1 ..." with a new line at each multiple of 8.
; Leaves A1 = the last address shown, so Return alone carries on [RM].
examine1:
        lda A1L
        sta A2L
        lda A1H
        sta A2H
dump:   jsr prhdr
@byte:  lda #$A0
        jsr COUT
        ldy #0
        lda (A1L),y
        jsr PRBYTE
        lda A1L
        cmp A2L
        lda A1H
        sbc A2H
        bcs @end
        inc A1L
        bne @nc
        inc A1H
@nc:    lda A1L
        and #$07
        bne @byte
        jsr CROUT
        jsr prhdr
        jmp @byte
@end:   jmp CROUT
prhdr:  lda A1H
        jsr PRBYTE
        lda A1L
        jsr PRBYTE
        lda #$AD
        jmp COUT

; Return alone [RM]: the next bytes, up to the next multiple of 8.
cont8:  inc A1L
        bne @nc
        inc A1H
@nc:    lda A1L
        ora #$07
        sta A2L
        lda A1H
        sta A2H
        jmp dump

gocall: jmp (A1L)               ; G [RM]: JSR to the address; its RTS comes back here

; MONZ [RM]: the "*" prompt and command loop. Understands:
;   addr          show one byte          addr.addr   show a range
;   Return alone  the next bytes         addr: bb bb store bytes (":" alone carries on)
;   addrG         run it (JSR)           I / N       inverse / normal text
;   Ctrl-Y        JMP to USRADR ($3F8)
;   Ctrl-B / Ctrl-C  BASIC, cold / warm start
; Anything else beeps and drops the rest of the line [RM].
monz_body:
        lda #$AA                ; '*'
        sta PROMPT
        jsr GETLNZ
        txa
        bne @parse
        jsr cont8
        jmp monz_body
@parse: lda #0
        sta MODE
        sta YSAV
@token: ldy YSAV
        jsr getnum
        sty YSAV
        lda NUMDIG
        beq @char
        jsr gotnum
@char:  ldy YSAV
        lda IN,y
        cmp #$8D
        beq @eol
        inc YSAV
        cmp #$A0
        beq @token
        cmp #$AE                ; '.'
        beq @dot
        cmp #$BA                ; ':'
        beq @colon
        cmp #$C7                ; 'G'
        beq @go
        cmp #$C9                ; 'I'
        beq @inv
        cmp #$CE                ; 'N'
        beq @norm
        cmp #$82                ; Ctrl-B [RM]: BASIC, cold start
        beq @basic
        cmp #$83                ; Ctrl-C [RM]: BASIC, warm start
        beq @bascon
        cmp #$99                ; Ctrl-Y
        beq @user
        jsr BELL
        jmp monz_body
@basic: jmp XBASIC
@bascon: jmp BASCONT
@dot:   sta MODE                ; A is still '.'
        bcs @token              ; always: carry is set by the equal compare
@colon: lda MODE
        cmp #$01
        bne @setc
        lda A1L
        sta A3L
        lda A1H
        sta A3H
@setc:  lda #$BA
        sta MODE
        bne @token
@go:    jsr gocall
        lda #0
        sta MODE
        beq @token
@inv:   jsr SETINV
        jmp @token
@norm:  jsr SETNORM
        jmp @token
@user:  jsr USRADR
        jmp monz_body
@eol:   lda MODE
        cmp #$01
        bne @again
        jsr examine1
@again: jmp monz_body

; ======================================================================
; $F800-$FFFF: documented entry points, in address order
; ======================================================================
        .segment "MONITOR"
        .org $F800

        fence $F800             ; PLOT
        fence $F80E             ; PLOT1
        fence $F819             ; HLINE
        fence $F828             ; VLINE
        fence $F832             ; CLRSCR
        fence $F836             ; CLRTOP
        fence $F847             ; GBASCALC
        fence $F85F             ; NXTCOL
        fence $F864             ; SETCOL
        fence $F871             ; SCRN
        fence $F882             ; INSDS1
        fence $F88E             ; INSDS2
        fence $F8D0             ; INSTDSP
        fence $F940             ; PRNTYX
        fence $F941             ; PRNTAX
        fence $F944             ; PRNTX

; PRBLNK [RM]: three blanks (forced: LDX #3 falls into PRBL2).
        at $F948
PRBLNK: ldx #$03
        .assert * = $F94A, error, "PRBL2 moved"
; PRBL2 [RM]: X blanks.
PRBL2:  jmp prbl2_body

        fence $F953             ; PCADJ
        fence $FA40             ; IRQ (Apple's own handler entry)
        fence $FA4C             ; BREAK
        fence $FA59             ; OLDBRK

        at $FA62
RESET:  jmp reset_body

        fence $FAA6             ; PWRUP
        fence $FABA             ; SLOOP
        fence $FAD7             ; REGDSP

; PREAD [RM] at $FB1E. Machine ID byte $FB1E = $AD on a ][ Plus [TN7]:
; LDA PTRIG is $AD, so the ID falls out of the routine itself.
        at $FB1E
PREAD:  lda PTRIG
        jmp pread_rest

        at $FB2F
INIT:   jmp init_body
        at $FB39
SETTXT: jmp init_body

        fence $FB40             ; SETGR
        fence $FB4B             ; SETWND
        fence $FB5B             ; TABV
        fence $FB60             ; APPLEII
        fence $FB6F             ; SETPWRC

; Machine ID byte [TN7]: $EA = Apple ][ Plus.
        at $FBB3
        .byte $EA

        at $FBC1
BASCALC: jmp bascalc_body
        at $FBDD
BELL1:  jmp bell1_body

        fence $FBF4             ; ADVANCE
        fence $FBFD             ; VIDOUT
        fence $FC10             ; BS
        fence $FC1A             ; UP

; VTAB [RM] (forced: LDA CV falls into VTABZ, two bytes later).
        at $FC22
VTAB:   lda CV
        .assert * = $FC24, error, "VTABZ moved"
VTABZ:  jmp vtabz_body

        fence $FC2C             ; ESC1

        at $FC42
CLREOP: jmp clreop_body
        at $FC58
HOME:   jmp home_body

        fence $FC62             ; CR
        fence $FC66             ; LF

        at $FC70
SCROLL: jmp scroll_body

; CLREOL [RM] (forced: LDY CH falls into CLEOLZ, two bytes later).
        at $FC9C
CLREOL: ldy CH
        .assert * = $FC9E, error, "CLEOLZ moved"
CLEOLZ: jmp cleolz_body

; WAIT [RM]: delay of 1/2 (26 + 27A + 5A^2) microseconds; A is 0 on return.
; This nested countdown is the smallest loop that meets the documented timing.
        at $FCA8
WAIT:   sec
@outer: pha
@inner: sbc #1
        bne @inner
        pla
        sbc #1
        bne @outer
        rts

; NXTA4 / NXTA1 [RM]: A4 + 1, then (NXTA1) compare A1 with A2 (carry set when
; A1 >= A2) and A1 + 1. Forced: NXTA4 is exactly six bytes before NXTA1.
        at $FCB4
NXTA4:  inc A4L
        bne NXTA1
        inc A4H
        .assert * = $FCBA, error, "NXTA1 moved"
NXTA1:  lda A1L
        cmp A2L
        lda A1H
        sbc A2H
        inc A1L
        bne @done
        inc A1H
@done:  rts

        fence $FCC9             ; HEADR
        fence $FCEC             ; RDBYTE
        fence $FCFA             ; RD2BIT

        at $FD0C
RDKEY:  jmp rdkey_body
        at $FD1B
KEYIN:  jmp keyin_body

        fence $FD2F             ; ESC

; RDCHAR [RM]: RDKEY with Esc editing. No Esc editing yet.
        at $FD35
RDCHAR: jmp RDKEY

; GETLNZ / GETLN / GETLN1 [RM] (forced: CR, then prompt, then the line).
        at $FD67
GETLNZ: jsr CROUT
        .assert * = $FD6A, error, "GETLN moved"
GETLN:  lda PROMPT
        jsr COUT
        .assert * = $FD6F, error, "GETLN1 moved"
GETLN1: jmp getln1_body

; CROUT1 [RM]: blank to the end of the line, then CROUT (forced).
        at $FD8B
CROUT1: jsr CLREOL
        .assert * = $FD8E, error, "CROUT moved"
CROUT:  lda #$8D
        bne COUT

        fence $FD92             ; PRA1
        fence $FDA3             ; XAM8

        at $FDDA
PRBYTE: jmp prbyte_body
        at $FDE3
PRHEX:  jmp prhex_body
; COUT [RM]: output through CSW (forced: exactly a 3-byte JMP before COUT1).
        at $FDED
COUT:   jmp (CSWL)
        .assert * = $FDF0, error, "COUT1 moved"
COUT1:  jmp cout1_body

        fence $FDF6             ; COUTZ

; MOVE [RM]: copy A1..A2 to A4 upward; Y must be 0 on entry [RM]. Forced: ten
; bytes before VFY.
        at $FE2C
MOVE:   lda (A1L),y
        sta (A4L),y
        jsr NXTA4
        bcc MOVE
        rts

        fence $FE36             ; VFY
        fence $FE5E             ; LIST
        fence $FE75             ; A1PC

; SETINV / SETNORM [RM]: INVFLG = $3F / $FF (forced: four bytes apart).
        at $FE80
SETINV: ldy #$3F
        bne setiflg
        .assert * = $FE84, error, "SETNORM moved"
SETNORM: ldy #$FF
setiflg: sty INVFLG
        rts

; SETKBD / SETVID [RM]: keyboard / screen as the I/O hooks (forced: they load
; slot 0 and fall into INPORT / OUTPORT, two bytes later).
        at $FE89
SETKBD: lda #0
        .assert * = $FE8B, error, "INPORT moved"
INPORT: jmp inport_body
        at $FE93
SETVID: lda #0
        .assert * = $FE95, error, "OUTPORT moved"
OUTPORT: jmp outport_body

; XBASIC / BASCONT [RM]: BASIC's cold and warm starts, through $E000 and $E003 (Apple II Reference
; Manual 1979: Ctrl-B "enter the language installed in ROM", program lost; Ctrl-C continue BASIC, kept).
        at $FEB0
XBASIC: jmp $E000
        .assert * = $FEB3, error, "BASCONT moved"
BASCONT: jmp $E003
        fence $FEB6             ; GO
        fence $FECA             ; USR
        fence $FECD             ; WRITE (tape)
        fence $FEFD             ; READ (tape)
        fence $FF2D             ; PRERR

; BELL [RM]: output Ctrl-G through COUT.
        at $FF3A
BELL:   lda #$87
        jmp COUT
        .assert * = $FF3F, error, "IOREST moved"
IOREST: jmp iorest_body
        at $FF4A
IOSAVE: jmp iosave_body
; IORTS [RM]: a known RTS; slot firmware does JSR IORTS to find its own slot.
        at $FF58
IORTS:  rts

        fence $FF59             ; OLDRST

; MON [RM]: beep, then MONZ (forced: CLD + JSR BELL is exactly four bytes).
        at $FF65
MON:    cld
        jsr BELL
        .assert * = $FF69, error, "MONZ moved"
MONZ:   jmp monz_body

        fence $FFA7             ; GETNUM
        fence $FFC7             ; ZMODE
        fence $FFCC             ; CHRTBL (command table)

; ---- 6502 vectors [RM] ----
        at $FFFA
        .word NMIV              ; NMI: through page 3
        .word RESET
        .word irq_body
