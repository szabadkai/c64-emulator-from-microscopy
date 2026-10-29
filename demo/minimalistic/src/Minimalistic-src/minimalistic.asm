
; Minimalistic 4K intro for CSDB.dk Intro Creation Competition 2019
;
; Code & Music by Proton / Finnish Gold

; Please accept my apologies about this source. It's real mess of spaghetti code.
; But it's just because of the intro compo. Everything is more looped rather than unrolled.
; also I had to move small parts of code all around during the development.
; there is also two "screens", starting screen with the bouncing raster, and the main screen.
; Many memory-areas are used twice or even more.
; This source compiles nice with 64tass, like:
;   64tass.exe -C -B -i -a minimalistic.asm -o minimalistic.prg
; Then run in VICE using:
;   x64.exe -autostartprgmode 1 -autostart-warp +truedrive +cart minimalistic.prg
; - Then invoke monitor using Alt+M and execute using command "g 800"
; - Either you can create self-extracting archive using exomizer with options "sfx 2048".
; To compile with tune, you will need the player+music data exported properly.
; The tune is composed using GoatTracker v2.75. You can export it for this intro by pressing F9
; >>> select all options to No, Start address: $0a00, $22-$23 for zeropage, save as BIN.

; memory usage:
; $02-$1f - main variables
; $30-$d4 - MCR-table (zeropage use is allowed by ICC rules)
; $22-$23 - GT2 music player registers
; $0100-$01ff - stack (stack area use allowed by ICC rules)
; ($0100-$017f - first charset for chessboard zoomer)
; ($0180-$01bf - first sprite used in the scroller)
; $0400-$0427 - First text row, stretched using FPP to cover entire screen
; $0428-$0437 - colortable
; $0440-$04fe - 3 more sprites used in the scroller
; $0500-$05ff - sinusdata-table, 256 bytes (generated using code)
; $0600-$06bf - 3 last sprites used in the scroller (total 7 sprites)
; $06c0-$07f5 - intro code
; $07f8-$07ff - sprite pointers
; $0800-$087f - intro init code/logo sprite+one empty sprite
; $0900-$097f - second charset for chessboard zoomer
; $0980-$0a01 - intro code
; $0a06-$115e - music player & with song data
; $115f-$13fb - intro code
; $d800-$d827 - chessboard color
; $d838-      - nybble scrolltexts (color ram memory use allowed by ICC rules)
; $db60-$dbb0 - chessboard rendering buffer

lines = 164
linecount = 2
temp = 2
scrollreg = 3
rbscale = 3
irqa = 4
irqx = 5
irqy = 6
textptr = 7 ; -8
fontptr = 9 ; -10
mul1 = 11
mul2 = 12
cb_cpixel = 13
cb_width = 14
cb_width_lo = 15
cb_cwidth = 16
cb_cwidth_lo = 17
cb_xpos = 18
scrollheight = 19
ff = 20
zero = 21
toplines = 22
xoffsets = 24 ; 8 values
sdelay = $ff
cb_buffer = $db60 
mcrtable = $30
csclist = $1c0
sinus = $0500
scrollcolors = $d828
text4b = scrolltext+$d400
runtext = text4b+(maintext-scrolltext)*2

        * = $0400
; data used to create deltatable for sinus-data
stable  .byte $34,$41,$3b,$21,$34,$21,$32,$21,$31,$21,$31,$22,$31,$29,$11
        .byte $22,$12,$21,$13,$21,$12,$01,$13,$01,$11,$02,$11,$03,0
; sprite xcoordinate offsets for the scroller
xoffs   .byte 8,8+48,8+2*48,8+3*48,8+4*48,8+5*48,8+6*48-256,8+7*48-256,0
zfont
        .byte %00000000       ; 0       bit table for creating the chessboard zoomer font
        .byte %10000000       ; 1
        .byte %11000000       ; 2
        .byte %11100000       ; 3
        .byte %11110000       ; 4
        .byte %11111000       ; 5
        .byte %11111100       ; 6
        .byte %11111110       ; 7
        .byte %11111111       ; 8
        .byte %01111111       ; 9
        .byte %00111111       ; 10
        .byte %00011111       ; 11
        .byte %00001111       ; 12
        .byte %00000111       ; 13
        .byte %00000011       ; 14
        .byte %00000001       ; 15
border  .byte 0
colors  .byte 6,2,4,8,10,5,15,7,1,7,15,5,10,8,4,2      ; colors for scroller flashing

logo
        .byte %11111111,%10111001,%11111110     ; at the first i didn't have any logo
        .byte %11100000,%00111011,%10000111     ; but it was one of the compo rules
        .byte %11100000,%00111011,%10000000     ; logo must exist, size doesn't matter
        .byte %11111110,%00111011,%10111111
        .byte %11100000,%00111011,%10000111
        .byte %11100000,%00111011,%10000111
        .byte %11100000,%00111001,%11111111

scrolltext
        .text "        MINIMALISTIC    ",254
maintext
        .text "        "
        .text "Finnish Gold proudly presents yet another 4K intro for CSDb "
        .text "Intro Creation Competition. Previous intro did not "
        .text "qualify due to special compo rules.    "
        .text "Intro code and tune by Proton of FIG.   "
        .text "Greetings to all Finnish Gold friends and fans.   "
        .text "Keep waiting for our comeback demo. It will be released soon!   "
        .text "Happy New Year 2020!     ",255

startfade
        sei
; convert scrolltext

textconv
        lda #<scrolltext ; convert normal scrolltext into two nybble pairs and store into the color ram
        sta 2
        sta 4
        sta textptr
        lda #>scrolltext
        sta 3
        lda #>runtext
        sta 5
        sta textptr+1
-       ldy #0
        sty $d011
        lda (2),y
        tax             ; save char
        and #15
        sta (4),y       ; store low nybble
        iny
        txa
        lsr
        lsr
        lsr
        lsr
        sta (4),y       ; store high nybble
        lda 4           ; move pointers
        clc
        adc #2
        sta 4           ; target by 2
        bcc +
        inc 5
+       inc 2           ; and source by 1
        bne +
        inc 3
+       inx             ; x = char, if 255 (+1 = 0)
        bne -
convdone

        lda $d020       ; save the border color
        sta border
        lda #$35        ; we don't need basic or kernal in intro
        sta 1           ; page changes here 
        jsr make_sinus  ; this creates sinus data over scrolltext and previous code

        lda $d011             ; raster vertical blanking
        bpl *-3
        lda $d011
        bmi *-3

        ldx #168              ; wait for the right start position after raster blanking
        dex
        bne *-1

        lda #<nmirq           ; setup NMI-timer IRQ
        sta $fffa
        lda #>nmirq
        sta $fffb
        lda #$c7
        sta $dd04
        lda #$4c
        sta $dd05
        lda #$81
        sta $dd0d
        lda #$11
        sta $dd0e
        bit $dd0d
        lda border            ; fill rastertable with current border color
fillzp  sta 64                ; we have full screen rastertable for smooth and solid operation
        inc fillzp+1          ; there are more than 256 rasterlines in use.
        bne fillzp            ; color table spans from zeropage to the stack
        ldx #90
fillst  sta $100,x
        dex
        bpl fillst
black = *+1
        lda #1
        bne *-2
        sta $dd0e            ; shut down the NMI Timer-IRQ
        sta $d020
        sta $d021
        lda #$4f
        sta $dd0d
        lda #<nmi            ; NMI stays served during the intro
        ldx #>nmi
        sta $fffa
        stx $fffb
        lda #2               ; init some variables for the intro
        sta scrollheight
        ldx #$ff             ; this is for 3cycle lda #$ff
        stx ff
        inx
        stx zero             ; and 3cycle lda #0
        txa
        ldx #39              ; start intro wirh checkboard not visible
-       sta $d800,x
        dex
        bpl -
        sta $d01b

        ldx #15
-       lda colors,x        ; move colortable to colorram
        sta scrollcolors,x
        lda xoffs,x
        sta xoffsets,x
        dex
        bpl -

        ldx #lines+1
        lda #0
-       sta mcrtable-1,x
        dex
        bne -
        stx $d01c
        stx $d01b
        dex        
        stx csclist
        rts


        
        * = $06c0

stabler ldx #7          ;small waitingloop to stable raster interrupt
        dex
        bne *-1
        lda $d012
        cmp $d012
        beq *+2
        nop
        nop
        nop
        rts

irq1    sta irqa        ; save registers. not really necessary to save them all
        stx irqx
        sty irqy
        inc $d019       ; acknowledge irq status
        nop
        nop
        nop
        jsr stabler     ; waste 3 rasterlines trying to stable the raster
        jsr stabler
        jsr stabler
sindex = *+1
        ldx #0          ; initial scroller y-position
        lda sinus,x     ; read sinus for scroller y-position
        lsr
        tay
        clc
        adc #90
        sta spr8y
        iny
        sty linecount
        txa
smove = *+1
        adc #0
        sta sindex

        lda #5
        sta toplines
        ldx #11
        dex
        bne *-1
        nop
        nop
        ldy #0
        beq fppskip0
fpploop0
        stx $d017       ; sprite y-exp off
        iny
fppskip0
        lda $d012
        and #7
        ora #$10
        ldx #$7f
        stx $d017
        ldx mcrtable,y ; read MCR value to set after FPP
        bne setmcr0
nomcr0  sta $d011,x
        nop
        ldx #0
        dec toplines
        beq hidelogo
        bne fpploop0
setmcr0
        sta $d011
        stx $d018
        ldx #0
        dec toplines
        bne fpploop0
hidelogo
        lda #$840/64
        stx $d017
        sta $07ff
        lda #$12
        bne fppskip1
fpploop1
        iny
        stx $d017       ; sprite y-exp off
        lda $d012
        and #7
        ora #$10
fppskip1
        dec $d017
        ldx mcrtable-1,y ; read MCR value to set after FPP
        bne setmcr1
nomcr1  sta $d011,x
        nop
        nop
        ldx #0
        dec linecount   ; continue until we hit the line where we want to have
        bne fpploop1    ; the scroller
        lda scrollheight ; start second FPP-loop, 34 lines for the scroller
        sta linecount
        bne fppskip2    ; adjust timing between loops
setmcr1 sta $d011
        stx $d018
        ldx #0
        dec linecount    ; continue until we hit the line where we want to have
        bne fpploop1    ; the scroller
        lda scrollheight ; start second FPP-loop, 34 lines for the scroller
        sta linecount
        bne fppskip2    ; adjust timing between loops
fpploop2
        nop
spr8y = *+1
        lda #0
        sta $d00f
fppskip2
        lda $d012
        and #7
        ora #$10
        sty $d022
        ldx mcrtable,y ; read MCR value to set after FPP
        bne setmcr2
nomcr2  sta $d011,x
        iny
        nop
        dec linecount
        bne fpploop2
        beq fppskip3
setmcr2 iny
        sta $d011
        stx $d018
        dec linecount
        bne fpploop2
        beq fppskip3
fpploop3
        inc $d017       ; sprite y-exp off
fppskip3
        lda $d012
        and #7
        ora #$10
        ldx #$ff
        stx $d017
        nop
        iny
        ldx mcrtable-1,y ; read MCR value to set after FPP
        bne setmcr3
nomcr3  sta $d011,x
        nop
        nop
        cpy #lines      ; continue until we have all lines done
        bne fpploop3
        beq fppdone
setmcr3 sta $d011
        stx $d018
        cpy #lines      ; continue until we have all lines done
        bne fpploop3
fppdone
        ldx #5
        dex
        bne *-1
        nop
        lda #$10
        sta $d016,x
screen_ctrl = *+1
        lda #$03
        sta $d011

        jsr chessboard
        jsr scroller
updater
        lda update

        ldy #51         ; wait line at the top of the screen
exitrirq
        sty $d012
        lda $dc0d       ; clear irq status
exitirq lda irqa        ; restore registers
        ldx irqx
        ldy irqy
nmi     rti             ; exit from irq

        * = $07f8

                .byte $0180/64,$0440/64,$0480/64,$04c0/64,$0600/64,$0640/64,$0680/64,$0800/64

        * = $0800       ; intro starts here

        jsr startfade   ; call bouncinig raster screen
        
        lda #$7f        ; setup raster interrupt for main intro
        sta $dc0d
        sta $dd0d
        lda #$03
        sta $d011
        ldy #48
        lda #<irq1
        ldx #>irq1
        sty $d012
        sta $fffe
        stx $ffff
        lda #1
        sta scrollreg
        sta $d019
        sta $d01a
        jmp runintro


nmirq   sta irqa        ; start screen raster uses NMI Timer IRQ
        stx irqx
        sty irqy
        ldy #0
        sty rthigh
        sty rbhigh
        sty rbhigh2
        lda #$40
        sta rtlow
        lda #$40+18
        sta rblow
        sta rblow2
        iny
        sty temp
        ldy #18
rtlow = *+1
rthigh = *+2
rasterlp
        lda $0100       ; single loop with variable waits 
        sta $d020
        inc rtlow 
        bne +
        inc rthigh
+       ldx #7
        dey
        bne rtwait2
        ldx #5
        dec temp
        beq rtwait2
        bmi rtdone
rtwait
        ldx #6
rtwait2
        dex
        bne *-1
        nop
        beq rasterlp
rtdone
        jsr bouncer     ; replay bouncing in the table
        lda rbscale
        beq +
        dec rbscale
+       sta black

        jsr music_play  ; play some sounds, other than just a drone sound
        lda $dd0d
        jmp exitirq

        .fill 8,0

; chessboard is constructed by variable width blocks, and for drawing we have
; 16 characters allowing us any pixel (0-7) starting and ending

; chessboard will rendered in to the buffer and then copied to the screen
; with max width sinus (8bit). 

; because we have 16 chars only (0=empty, 8=full, 1-7 pixels left, 9-15 right)

buffersize = 76

chessboard              ; render chessboard. start from the middle of buffer to right

        lda #8
        cld
        sta cb_buffer+buffersize/2
        ldx #buffersize/2+1
        ldy #buffersize/2       ; start from the middle of the buffer
        lda #0
        sta cb_cwidth
        sta cb_cwidth_lo
_block  lda cb_cwidth_lo        ; calculate low bits first to increase zoom accuracy
        adc cb_width_lo
        sta cb_cwidth_lo
        lda cb_cwidth
        adc cb_width
        sta cb_cwidth
-       lda cb_cwidth
        cmp #8
        bcc +
        sbc #8
        sta cb_cwidth
        lda #8
        sta cb_buffer,x
        inx
        dey
        bpl -
        bmi _done 
+       dex
        sta cb_buffer,x
        inx
        dey
        bmi _done 
        sta cb_cwidth            ; upper loop renders blocks, this does spaces
        lda cb_cwidth_lo
        adc cb_width_lo
        sta cb_cwidth_lo
        lda cb_cwidth
_space  adc cb_width
        sta cb_cwidth
-       lda cb_cwidth
        cmp #8
        bcc +
        sbc #8
        sta cb_cwidth
        lda #0
        sta cb_buffer,x
        inx
        dey
        bpl -
        bmi _done 
+       dex
        eor #8
        sta cb_buffer,x
        inx
        dey
        bmi _done 
        and #7
        sta cb_cwidth
        jmp _block
_done
        jmp mirror_cb           ; once chessboard is rendered, copy the reversed mirror of it


        * = $0a00  ; we put tune in place first. there are 2 jump vectors at the start
music_play = $0ad0 ; we call player by direct address, allowing intro code overwrite tune vectors
        .binary "minimalistic.bin"

scroller        
        lda scrollreg   ; move scroller
        sec
scrollspeed = *+1
        sbc #4
        sta scrollreg
        bcc shiftscroll
        and #8
        bne +
        lda #%11000000  ; sprites 9th bits
        sta x9b
+       rts
shiftscroll
        adc #16         ; next character.. shift sprite contents
        sta scrollreg
        lda #%11100000  ; if scrollreg > 8, 3 sprite xcoords is over 256
        sta x9b
        ldx #9          ; start from 4th sprite row
        ldy #8          ; move 8 lines of spritedata

-       lda $181,x      ; 20 columns
        sta $180,x
        lda $182,x
        sta $181,x
        lda $440,x
        sta $182,x
        lda $441,x
        sta $440,x
        lda $442,x
        sta $441,x
        lda $480,x
        sta $442,x
        lda $481,x
        sta $480,x
        lda $482,x
        sta $481,x
        lda $4c0,x
        sta $482,x
        lda $4c1,x
        sta $4c0,x
        lda $4c2,x
        sta $4c1,x
        lda $600,x
        sta $4c2,x
        lda $601,x
        sta $600,x
        lda $602,x
        sta $601,x
        lda $640,x
        sta $602,x
        lda $641,x
        sta $640,x
        lda $642,x
        sta $641,x
        lda $680,x
        sta $642,x
        lda $681,x
        sta $680,x
        lda $682,x
        sta $681,x
        txa
        clc
        adc #6          ; step 2 lines. leave one blank line after each line for style
        tax 
        dey
        bne -           ; y = 0
        ldx #$d8/2      ; locase ROM at $d800
        iny
        lda (textptr),y ; read scrolltext (high nybble)
        asl
        asl
        asl
        asl
        sta 2           ; save temp
        dey
        lda (textptr),y ; read scrolltext (low nybble)
        and #15
        ora 2           ; combine nybbles
        cmp #$fe
        bne _nostop
        lda #0
        sta scrollspeed
        lda #$53
        sta screen_ctrl ; enable screen on next frame
        lda #$20 ; JSR
        sta updater
_nostop
        cmp #$ff        ; end of texts?
        bne _noend
        lda #<runtext 
        sta textptr
        lda #>runtext
        sta textptr+1
_space
        lda #" "
_noend
        bpl _nocaps     ; check upper/lowercase
        ldx #$d0/2      ; upcase ROM at $d000
_nocaps 
        stx fontptr+1   ; font pointer msb
        and #$3f        ; mask extra bits off to get charcode
        asl             ; *8
        asl
        asl
        rol fontptr+1
        sta fontptr

        inc textptr  ; update scrollptr
        inc textptr
        bne +
        inc textptr+1
+
        lda #$33        ; make char ROM visible
        sta 1 
        ldx #9          ; 4th sprite row again
-       lda (fontptr),y ; copy font data from ROM
        sta $682,x
        txa
        clc
        adc #6
        tax 
        iny
        cpy #8
        bne -
        lda #$35        ; once copied, hide ROMS again
        sta 1
        rts

mirror_cb
        ldx #buffersize/2       ; rendering part calculates right side of the chessboard
        ldy #buffersize/2-1     ; into the buffer. this loop creates an inverter mirror of it
-       lda cb_buffer,x
        and #15
        beq +
        cmp #8
        beq +
        eor #7
        sta cb_buffer,y
        inx
        dey
        bpl -
        bmi _mirrored
+       eor #8
        sta cb_buffer,y
        inx
        dey
        bpl -
_mirrored
        jmp music_play          ; once done, play some sweet sounds


cb_moves

cbzs = *+1
        lda #0          ; sinus pointer for zoom (size)
        clc
        cld
        adc #1
        and #$7f        ; only use half-sinus (upper half)
        sta cbzs
        tax
        lda sinus,x     ; get the size of the chessboard block
        sec
        sbc #120       ; adjust half sinus zero-level
        ldx #200
        jsr multiply
        clc
        adc #50         ; adjust the minimum values little bit bigger

        ldx #0
        stx cb_width_lo ; we don't want too big blocks,
        lsr             ; so we shift sinus value a bit smaller
        ror cb_width_lo ; and to increase accuracy, we save shifted bits
        sta cb_width    ; block width in pixels

        txa             ; x = 0
-       ldy csclist,x   ; read previous MCR-value settings from list and clear
        cpy #255        ; this is lots of faster than emptying entire table every frame
        beq +           ; $ff is out of range, so it indicates the end of list.
        sta mcrtable,y  ; block size varies, so does the amount of values in the list
        inx
        bne -
+
        lda #$10        ; reset MCR-registers for both loops
        sta cc1         ; we will use two loops because we have variable centerline
        lda #$12        ; and first loop counts the values to and of the table
        sta cc2         ; and second loop obviously does the same to the beginning
cbys = *+1
        lda #0          ; sinus pointer for y-movement
        clc
        adc #3          ; speed of y-movement. this is a bit faster moving than x or z
        sta cbys
        tax
        lda sinus,x     ; get sinus value for y-movement
        lsr             ; scale sinus a bit smaller
        clc
        adc #30         ; add some margin to the starting line
        sta temp        ; this is centerline, remember it for the second loop also
        ldx #0          ; initialize mcr-setting list index
        stx cb_cwidth_lo; also init low bits of the current width for better accuracy
ccloop  cmp #lines      ; ensure that value is inside boundary
        bcs _done1      ; if too high, first loop is ready
        sta csclist,x   ; save the offset, c-flag remains 0
        inx
        tay             ; use this offset
cc1 = *+1
        lda #$10        ; this is mcr-value of the current charset
        sta mcrtable,y  ; set mcr-value in the table
        eor #2          ; toggle the other charset, we use 2 ($0000-$07ff and $0800-$0fff)
        sta cc1
        lda cb_cwidth_lo; calculate low bits of the current width, c-flag doesn't matter
        adc cb_width_lo ; possible low-bit values are 64-192. one doesn't matter
        sta cb_cwidth_lo; adc set C-flag for the next adc, increasing the accuracy
        tya             ; current offset
        adc cb_width    ; move offset by block width
        bcc ccloop      ; check C-flag after addition. if offset didn't overrun, keep looping
_done1  lda #0          ; reset current width low bits again
        sta cb_cwidth_lo
        lda temp        ; re-use same starting as previously (loop 1 went downwards on screen)
                        ; c-flag is 1
ccloop2 sbc cb_width    ; loop2 goes upwards
        beq _done2      ; if we hit zero-offset (topmost value) we're ready
        bcc _done2      ; also if offset-value underruns
        sta csclist,x   ; save the offset to the list
        inx
        tay             ; use this offset
cc2 = *+1
        lda #$12        ; like in loop1 we set new mcr-values in the table
        sta mcrtable,y
        eor #2          ; and toggle between two charsets
        sta cc2
        lda cb_cwidth_lo; and like in loop1 count low bits first
        sbc cb_width_lo ; if this underruns, we can use C-flag status for better accuracy
        sta cb_cwidth_lo
        tya             ; current offset
        jmp ccloop2     ; keep looping...
_done2
        lda #255        ; list is done, flag end by $ff
        sta csclist,x
        lda cc2         ; read the latest charset mcr-value for the first value to set in fpp-loop
        sta mcrtable
        sta $d018       ; also use the same charset for the lines before our fpp-loop start
        rts

update 

ci = *+1
        lda #0                 ; this is just a simple scrolltext flasher
        and #15
        tax
        lda scrollcolors,x     ; read color for scroller
        sta sprcolor
        inc ci 

        ldy #38                 ; this shifts the chessboard color from left to right
-       lda $d800,y
        sta $d801,y
        dey
        bpl -

        inc sdelay              ; wait a bit with the black screen before showing the chessboard
        bne nofx
cbcolor = *+1
        lda #%1011              ; switch chessboard color between gray and blue
        eor #%1101
        sta cbcolor
        sta $d800
nofx

spos = *+1
        ldx #2
        bmi skipfx
        inc spos
        cpx #66
        bcs +
        txa
        lsr
        sta scrollheight        ; this is small effect that does stretch the scroller
        lda sinus+128,x
        sta logox
+       cpx #100
        bne nospeed
        lda #2
        sta scrollspeed
nospeed cpx #120
        bne skipfx
        lda #1
        sta smove               ; slide the "logo" in
skipfx

cbxs = *+1
        ldx #0          ; sinus pointer for x-movement
        inx
        stx cbxs
        lda sinus,x     ; get the size of the chessboard block
        tax
        and #7
        eor #7
        sta $d016
        txa
        lsr
        lsr
        lsr
        sta cb_xpos

        ldy cb_xpos
        ldx #0          ; copy buffer on screen
cbcopy  lda cb_buffer+4,y ; read from buffer (4bit ram)
        and #$0f
        ora #$20        ; use characters $20-2f from charsets
        sta $0400,x
        iny
        inx
        cpx #40
        bne cbcopy

        ldx #0
        ldy #0
-       lda #55         ; set sprites to top of the screen
        sta $d001,x
        lda scrollreg   ; move sprites based on "scroll register"
        clc
        adc xoffsets,y  ; add offset for each sprite x-coordinate
        sta $d000,x
sprcolor = *+1
        lda #0          ; and set colors
        sta $d027,y
        inx
        inx
        iny
        cpy #8
        bne -
        ldx #0          ; disable y-expanding
        stx $d017
        dex
        stx $d015       ; show all sprites
        stx $d02e       ; make logo light grey
        stx $d01d       ; and all enable x-expanding
x9b = *+1
        lda #0          ; sprite x-coord 9-bits
        sta $d010
logox = *+1
        lda #88
        adc #24
        sta $d00e       ; place "logo" on the right top corner
        lda #$800/64
        sta $07ff

        jmp cb_moves
        
multiply
        sta mul1        ; multiply value mul1 by value mul2 
        stx mul2
        lda #0
        ldx #8
        clc
-       bcc +
        clc 
        adc mul2
+       ror
        ror mul1
        dex
        bpl -
        ldx mul1        ; result in accu+xr
        rts        

        * = $0900

make_sinus

        ldx #0          ; nasty routine, 256b sinus in 96b code+data
        ldy #0
mksin1  lda stable,y    ; feel free to find out how it works
        beq _dtok       ; create curve until 0
        pha
        and #15         ; extract repeat counter
        sta 2
        pla
        lsr
        lsr
        lsr
        lsr
-       sta $100,x      ; temporarily fill stack to delta-table
        inx
        dec 2
        bne -
        iny             ; next data
        bne mksin1      ; branch always as data is very small
_dtok   ldy #0
        ldx #64
        lda #128        ; middle-value
mksin2  sta sinus,y     ; render positive rising curve
        sta sinus+64,x  ; falling curve backwards
        pha
        and #$7f
        sta 2           ; save current level
        lda #128
        sec
        sbc 2           ; flip to negative level
        sta sinus+128,y ; and render negative curves
        sta sinus+192,x
        pla             ; restore original current value
        clc
        adc $0100,y     ; apply delta
        iny             ; iterate until done.
        dex
        bpl mksin2
        stx rbscale     ; initialize rasterbouncer scale to maximum
        rts

bouncer
rbsin = *+1             ; this is bouncing rasterbar at the intro start
        ldx #64         ; boring effect, but i had no choise because limited memory
        txa            
rbstep = *+1            
        adc #2
        sta rbsin
        and #$7f        ; actually limit the sinus to the halfsine (bouncing effect)
        tax
        bcc +
        inc rbstep              
        lda rbscale     ; also rise up the scale down the step.
        beq noscale
        sbc #5                  
        sta rbscale
noscale
        lda rblines
        cmp #40
        bcs +
        adc #4
        sta rblines
+       lda sinus,x
        sec
        sbc #128
        asl 
        ldx rbscale
        jsr multiply
        sta temp
        lda #255
        sec
        sbc temp
        tay
rblines = *+1
        ldx #21
rblow = *+1
rbhigh = *+2
-       sta 1234,y
        lda border
rblow2 = *+1
rbhigh2 = *+2
        sta 1234,y
        lda #0
        dec rblow
        inc rblow2
        dex
        bne -
        rts

runintro
        ldx #lines
        lda #0
-       sta mcrtable-1,x
        dex
        bne -
        tay
-       lda logo,x              ; copy fig logo
        sta $800,x 
        tya
        sta $815,x              ; and clear below
        inx
        cpx #$15
        bne -
        ldx #64
-       sta $180-1,x            ; clear scroll sprites
        sta $4c0-1,x
        sta $680-1,x
        sta $840-1,x            ; clear also one sprite after logo
        dex
        bne -
-       sta $440,x              ; clear more sprites
        sta $600,x
        inx
        bpl -

        tax
        tay
mkflp   sty temp
        lda zfont,y           ; read char bits
        ldy #8
-       sta $0900,x           ; build font
        pha
        eor #$ff
        sta $0100,x           ; inverse to second charset
        pla
        inx
        dey                   ; repeat 8 times to fill entire char
        bne -
        ldy temp
        iny
        cpy #16               ; create 16 chars
        bne mkflp

        lda $dc0d       ; clear/ack irq status
        cli 

forever
        lda $dc01       ; read keyboard
        cmp #$ef
        bne forever
        sei       
        lda #$37        ; roms on
        sta 1
        lda #0
        sta $d020
        sta $d011
        jmp $fce2       ; exit with reset. this overlaps tune already


