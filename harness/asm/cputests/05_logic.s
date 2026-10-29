;! name: shifts/rotates, BIT, CMP/CPX/CPY, INC/DEC, abs,X across a page
;! mem $0300 = $02
;! mem $0301 = $05
;! mem $0302 = $02
;! mem $0303 = $81
;! mem $0304 = $30
;! mem $0305 = $C0
;! mem $0306 = $C2
;! mem $0307 = $03
;! mem $0308 = $80
;! mem $0309 = $00
;! mem $030A = $FF
;! mem $030B = $5A
;! mem $030C = $A5
        .org $0200
start:  LDX #$FF
        TXS
        LDA #$81
        ASL A           ; $02 C=1
        STA $0300
        ROL A           ; $05 C=0
        STA $0301
        LSR A           ; $02 C=1
        STA $0302
        ROR A           ; $81 C=0
        STA $0303
        LDA #$C0
        STA $10
        LSR $10         ; $60 C=0
        ROR $10         ; $30
        LDA $10
        STA $0304
        LDA #$C3
        STA $11
        LDA #$01
        BIT $11         ; N=1 V=1 Z=0
        PHP
        PLA
        AND #$C2
        STA $0305
        LDA #$3C
        BIT $11         ; N=1 V=1 Z=1
        PHP
        PLA
        AND #$C2
        STA $0306
        LDX #$40
        CPX #$40        ; Z=1 C=1
        PHP
        PLA
        AND #$83
        STA $0307
        LDY #$10
        CPY #$20        ; N=1 C=0
        PHP
        PLA
        AND #$83
        STA $0308
        LDA #$FF
        STA $12
        INC $12         ; $00
        DEC $13         ; $00 -> $FF
        LDA $12
        STA $0309
        LDA $13
        STA $030A
        LDX #$10
        LDA #$5A
        STA $03F8,X     ; -> $0408
        LDA $0408
        STA $030B
        LDA #$5A
        EOR #$FF        ; $A5
        ORA #$01        ; $A5
        AND #$FF
        STA $030C
done:   JMP done
