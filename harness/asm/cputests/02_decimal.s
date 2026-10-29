;! name: decimal mode ADC/SBC
;! mem $0300 = $47
;! mem $0301 = $29
;! mem $0302 = $00
;! mem $0303 = $01
;! mem $0304 = $90
        .org $0200
start:  LDX #$FF
        TXS
        SED
        CLC
        LDA #$19
        ADC #$28        ; 19+28 = 47
        STA $0300
        SEC
        LDA #$42
        SBC #$13        ; 42-13 = 29
        STA $0301
        CLC
        LDA #$99
        ADC #$01        ; 99+1 = 00, C=1
        STA $0302
        LDA #$00
        ADC #$00        ; + carry = 01
        STA $0303
        SEC
        LDA #$00
        SBC #$10        ; 00-10 = 90 (borrow)
        STA $0304
        CLD
done:   JMP done
