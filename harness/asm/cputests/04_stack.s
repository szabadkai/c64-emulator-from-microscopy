;! name: stack, JSR/RTS nesting, BRK/RTI through the IRQ vector
;! mem $0300 = $FD
;! mem $0301 = $22
;! mem $0302 = $11
;! mem $0303 = $06
;! mem $0304 = $77
;! mem $0305 = $FF
;! mem $0306 = $F9
;! mem $0307 = $30
;! s = $FF
        .org $0200
start:  LDX #$FF
        TXS
        LDA #<brkh      ; IRQ/BRK vector lives in (flat) RAM in this test
        STA $FFFE
        LDA #>brkh
        STA $FFFF
        LDA #$11
        PHA
        LDA #$22
        PHA
        TSX
        STX $0300
        JSR sub1
        PLA
        STA $0301
        PLA
        STA $0302
        BRK
        .byte $EA       ; BRK skips this byte
        STA $0304       ; A set by the handler
        TSX
        STX $0305
        JMP done
sub1:   JSR sub2
        INC $0303
        RTS
sub2:   TSX
        STX $0306
        LDA #$05
        STA $0303
        RTS
brkh:   PLA             ; status pushed by BRK: B and bit 5 set
        PHA
        AND #$30
        STA $0307
        LDA #$77
        RTI
done:   JMP done
