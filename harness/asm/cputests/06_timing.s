;! name: cycle counts between opcode fetches (page-cross and branch penalties)
;! cycles t0 t1 = 55
;! cycles t1 t2 = 8
        .org $0200
start:  LDX #$FF
        TXS
        LDA #$FF        ; pointer $20 -> $02FF (crosses to $0300 with Y=1)
        STA $20
        LDA #$02
        STA $21
t0:     LDA #$01        ; 2
        STA $10         ; 3
        LDA $10         ; 3
        STA $0300       ; 4
        LDX #$FF        ; 2
        LDA $02F0,X     ; 5 (page crossed)
        LDY #$01        ; 2
        LDA ($20),Y     ; 6 (page crossed)
        INX             ; 2 (X=0, Z=1)
        BNE t0          ; 2 (not taken)
        BEQ skip1       ; 3 (taken, same page)
skip1:  JSR sub         ; 6
        PHA             ; 3
        PLA             ; 4
        NOP             ; 2      (+ RTS 6) = 55
t1:     INC $0400       ; 6
        CLC             ; 2  -> 8
t2:     NOP
done:   JMP done
sub:    RTS
