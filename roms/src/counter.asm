; counter.asm - counts 0 to 255 and back to 0, in decimal.
;
; Exercises Fx33 (binary-coded decimal) and Fx65 together, which is the usual
; way a CHIP-8 program turns a byte into digits it can draw.
;
; The awkward part is that Fx65 always loads starting at V0, so reading the
; three BCD digits back necessarily clobbers the counter. V4 holds a copy
; across the load, chosen because Fx65 with x = 2 only ever writes V0 to V2.
;
;   V0-V2 BCD digits (and V0 doubles as the counter between iterations)
;   V4    saved counter      V5 draw x      V6 draw y      V7 frame delay

start:
    LD   V0, 0

loop:
    CLS
    LD   I, digits
    LD   B, V0          ; write hundreds, tens, units to digits[0..2]
    LD   V4, V0         ; stash the counter before Fx65 overwrites V0
    LD   I, digits
    LD   V2, [I]        ; V0 = hundreds, V1 = tens, V2 = units

    LD   V5, 22         ; three 4-wide glyphs, 5 apart
    LD   V6, 13
    LD   F, V0
    DRW  V5, V6, 5
    ADD  V5, 5
    LD   F, V1
    DRW  V5, V6, 5
    ADD  V5, 5
    LD   F, V2
    DRW  V5, V6, 5

    LD   V0, V4         ; restore and advance; wraps 255 to 0 on its own
    ADD  V0, 1

    LD   V7, 12         ; roughly five counts a second
    LD   DT, V7
wait:
    LD   V7, DT
    SE   V7, 0
    JP   wait

    JP   loop

digits:
    DB 0, 0, 0
