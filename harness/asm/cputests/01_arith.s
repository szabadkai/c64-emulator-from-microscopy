;! name: ADC/SBC binary, N/V/Z/C flags via PHP
;! mem $0300 = $A0
;! mem $0301 = $F4
;! mem $0302 = $60
;! mem $0303 = $34
;! mem $0304 = $00
;! mem $0305 = $37
;! mem $0306 = $7F
;! mem $0307 = $75
;! mem $0308 = $00
;! mem $0309 = $15
        .org $0200
start:  LDX #$FF
        TXS
        CLD
        CLC
        LDA #$50
        ADC #$50        ; $A0: N=1 V=1 Z=0 C=0
        STA $0300
        PHP
        PLA
        STA $0301       ; NV1B D I Z C = 1111 0100 (I set by reset)
        SEC
        LDA #$50
        SBC #$F0        ; $60, borrow -> C=0, V=0
        STA $0302
        PHP
        PLA
        STA $0303
        CLC
        LDA #$FF
        ADC #$01        ; $00 Z=1 C=1
        STA $0304
        PHP
        PLA
        STA $0305
        SEC
        LDA #$80
        SBC #$01        ; $7F V=1 C=1
        STA $0306
        PHP
        PLA
        STA $0307
        CLC             ; 16-bit $12FF + $0201 = $1500
        LDA #$FF
        ADC #$01
        STA $0308
        LDA #$12
        ADC #$02
        STA $0309
done:   JMP done
