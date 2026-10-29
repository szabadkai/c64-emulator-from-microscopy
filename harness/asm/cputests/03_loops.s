;! name: loops, indexed and (zp),Y addressing, shift-add multiply
;! mem $0300 = $BA
;! mem $0301 = $13
;! mem $0310 = $01
;! mem $0311 = $01
;! mem $0315 = $08
;! mem $031A = $59
;! mem $031B = $90
;! mem $0400 = $DE
;! mem $0403 = $EF
;! mem $040F = $5A
;! mem $0320 = $8F
;! x = $00
;! y = $0B
        .org $0200
start:  LDX #$FF
        TXS
        LDA #0          ; sum 1..100 = 5050 = $13BA
        STA $0300
        STA $0301
        LDX #100
sum:    TXA
        CLC
        ADC $0300
        STA $0300
        LDA $0301
        ADC #0
        STA $0301
        DEX
        BNE sum
        LDA #1          ; Fibonacci 1 1 2 3 5 8 13 21 34 55 89 144
        STA $0310
        STA $0311
        LDX #0
fib:    LDA $0310,X
        CLC
        ADC $0311,X
        STA $0312,X
        INX
        CPX #10
        BNE fib
        LDA #<table     ; copy 16 bytes through (zp),Y pointers
        STA $FB
        LDA #>table
        STA $FC
        LDA #$00
        STA $FD
        LDA #$04
        STA $FE
        LDY #15
copy:   LDA ($FB),Y
        STA ($FD),Y
        DEY
        BPL copy
        LDA #13         ; 13 * 11 = 143 by shift and add
        STA $20
        LDA #11
        STA $21
        LDA #0
        LDX #8
mul:    ASL A
        ASL $21
        BCC skip
        CLC
        ADC $20
skip:   DEX
        BNE mul
        STA $0320
        LDY #11
done:   JMP done
table:  .byte $DE,$AD,$BE,$EF,1,2,3,4,5,6,7,8,9,10,11,$5A
