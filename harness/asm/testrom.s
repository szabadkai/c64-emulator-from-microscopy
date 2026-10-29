; testrom.s - self-written 8 KB replacement KERNAL ($E000-$FFFF) that proves
; the machine works without Commodore ROMs.  It exercises: the netlist CPU,
; 6510 port banking, the PLA, VIC text mode from a RAM font, CIA1 timer IRQ,
; VIC raster IRQ (border colour split), badline stalls.
;
; build: python3 -I tools/asm6502.py rom asm/testrom.s build/testrom.bin E000 10000

SCREEN  = $0400
COLRAM  = $D800
FONTRAM = $3000
RL1     = 150           ; raster line: border -> red
RL2     = 20            ; raster line: border -> light blue

ciacnt  = $10           ; 16-bit CIA1 timer IRQ counter
rascnt  = $12           ; 16-bit raster IRQ counter
maincnt = $14           ; 16-bit main loop counter
rflag   = $16
ptr     = $FB           ; screen pointer
sptr    = $FD           ; string pointer

        .org $E000

reset:  SEI
        CLD
        LDX #$FF
        TXS
        LDA #$36        ; port data first: LORAM=0 HIRAM=1 CHAREN=1 -> RAM at $A000,
        STA $01         ; I/O at $D000, this ROM at $E000 (data must be set before
        LDA #$2F        ; the DDR turns P0-P2 into outputs, or the ROM vanishes)
        STA $00         ; 6510 DDR: P0-P3, P5 outputs

        LDA #$7F        ; CIAs: mask all interrupts, stop timers, ack
        STA $DC0D
        STA $DD0D
        LDA #$00
        STA $DC0E
        STA $DC0F
        STA $DD0E
        STA $DD0F
        LDA $DC0D
        LDA $DD0D
        LDA #$03        ; CIA2 PA0/PA1 outputs = %11 -> VIC bank 0 ($0000-$3FFF)
        STA $DD02
        STA $DD00
        LDA #$FF        ; CIA1 PA = keyboard columns (out), PB = rows (in)
        STA $DC02
        LDA #$00
        STA $DC03

        LDA #$1B        ; VIC: DEN, 25 rows, YSCROLL=3
        STA $D011
        LDA #$08        ; 40 columns
        STA $D016
        LDA #$1C        ; screen $0400, character set $3000
        STA $D018
        LDA #$0E
        STA $D020
        LDA #$06
        STA $D021
        LDA #$00
        STA $D01A
        LDA #$0F
        STA $D019

        LDX #0          ; copy the 512-byte font to RAM at $3000
cfont:  LDA font,X
        STA FONTRAM,X
        LDA font+256,X
        STA FONTRAM+256,X
        INX
        BNE cfont

        LDX #0          ; clear screen (spaces) and colour RAM (white)
cls:    LDA #$20
        STA SCREEN,X
        STA SCREEN+$100,X
        STA SCREEN+$200,X
        STA SCREEN+$2E8,X
        LDA #$01
        STA COLRAM,X
        STA COLRAM+$100,X
        STA COLRAM+$200,X
        STA COLRAM+$2E8,X
        INX
        BNE cls

        LDA #$07        ; title in yellow
        LDX #39
ctitle: STA COLRAM+40,X
        DEX
        BPL ctitle

        LDA #0
        STA ciacnt
        STA ciacnt+1
        STA rascnt
        STA rascnt+1
        STA maincnt
        STA maincnt+1
        STA rflag

        LDX #0          ; print the static texts (table of screen pos / string)
ptext:  LDA texts,X
        STA ptr
        LDA texts+1,X
        STA ptr+1
        BEQ pdone
        LDA texts+2,X
        STA sptr
        LDA texts+3,X
        STA sptr+1
        JSR print
        TXA
        CLC
        ADC #4
        TAX
        BNE ptext
pdone:
        LDA #<(SCREEN+40*7+11)   ; 6510 port read-back: "PORT $00=2F $01=36"
        STA ptr
        LDA #>(SCREEN+40*7+11)
        STA ptr+1
        LDY #0
        LDA $00
        JSR hex2
        LDY #7
        LDA $01
        JSR hex2

        LDA #$25        ; CIA1 timer A = $4025 (PAL jiffy), continuous, IRQ
        STA $DC04
        LDA #$40
        STA $DC05
        LDA #$81
        STA $DC0D
        LDA #$11        ; start + force load
        STA $DC0E

        LDA #RL1        ; VIC raster IRQ
        STA $D012
        LDA #$1B
        STA $D011
        LDA #$01
        STA $D01A
        CLI

main:   INC maincnt
        BNE m1
        INC maincnt+1
m1:     LDA #<(SCREEN+40*3+19)
        STA ptr
        LDA #>(SCREEN+40*3+19)
        STA ptr+1
        LDY #0
        LDA ciacnt+1
        JSR hex2
        LDA ciacnt
        JSR hex2
        LDY #40
        LDA rascnt+1
        JSR hex2
        LDA rascnt
        JSR hex2
        LDY #80
        LDA maincnt+1
        JSR hex2
        LDA maincnt
        JSR hex2
        JMP main

; hex2: print A as two hex digits at (ptr),Y; Y += 2
hex2:   PHA
        LSR A
        LSR A
        LSR A
        LSR A
        JSR nyb
        STA (ptr),Y
        INY
        PLA
        AND #$0F
        JSR nyb
        STA (ptr),Y
        INY
        RTS
nyb:    CMP #10
        BCC ndig
        SBC #9          ; carry is set: 10..15 -> screen codes 1..6 (A-F)
        RTS
ndig:   ORA #$30
        RTS

; print: screen codes from (sptr) to (ptr) until $FF
print:  LDY #0
pr1:    LDA (sptr),Y
        CMP #$FF
        BEQ pr2
        STA (ptr),Y
        INY
        BNE pr1
pr2:    RTS

irq:    PHA
        TXA
        PHA
        TYA
        PHA
        LDA $D019
        AND #$01
        BEQ notras
        STA $D019       ; acknowledge raster IRQ
        INC rascnt
        BNE r0
        INC rascnt+1
r0:     LDA rflag
        EOR #$01
        STA rflag
        BEQ r2
        LDA #$02        ; lower part of the frame: red border
        STA $D020
        LDA #RL2
        STA $D012
        JMP notras
r2:     LDA #$0E        ; upper part: light blue border
        STA $D020
        LDA #RL1
        STA $D012
notras: LDA $DC0D       ; reading ICR acknowledges CIA1
        AND #$01
        BEQ iout
        INC ciacnt
        BNE iout
        INC ciacnt+1
iout:   PLA
        TAY
        PLA
        TAX
        PLA
nmi:    RTI

texts:  .word SCREEN+40*1+12, t_ok
        .word SCREEN+40*3+2,  t_cia
        .word SCREEN+40*4+2,  t_ras
        .word SCREEN+40*5+2,  t_main
        .word SCREEN+40*7+2,  t_port
        .word SCREEN+40*10+2, t_abc
        .word SCREEN+40*11+2, t_num
        .word SCREEN+40*13+2, t_info
        .word 0, 0

t_ok:   .scr "NETLIST 6502 OK"
        .byte $FF
t_cia:  .scr "CIA1 TIMER IRQS $"
        .byte $FF
t_ras:  .scr "RASTER IRQS     $"
        .byte $FF
t_main: .scr "MAIN LOOP       $"
        .byte $FF
t_port: .scr "PORT $00=   $01="
        .byte $FF
t_abc:  .scr "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        .byte $FF
t_num:  .scr "0123456789 !#$%&()*+,-./:;=?"
        .byte $FF
t_info: .scr "SELF-WRITTEN TEST ROM, NO CBM CODE"
        .byte $FF

font:
        .include "gen/font.s"

        .org $FFFA
        .word nmi, reset, irq
