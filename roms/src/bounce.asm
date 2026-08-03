; bounce.asm - a 4x4 ball bouncing around the screen.
;
; Paces itself off the display-wait quirk rather than the delay timer: with
; display_wait on, a draw is capped at one per frame, so the draw/erase pair
; below takes two frames and the ball moves at a steady 30 pixels a second
; without any explicit timing code.
;
; Bounds are checked by equality rather than magnitude because CHIP-8 has no
; signed compare. The ball moves one pixel at a time, so it can only ever
; overshoot an edge by exactly one, and testing for that single value is
; enough.
;
;   V0 ball x      V2 x velocity     V6 scratch
;   V1 ball y      V3 y velocity     V7 beep length

start:
    CLS
    LD   V0, 20         ; starting position
    LD   V1, 10
    LD   V2, 1          ; +1 right
    LD   V3, 1          ; +1 down
    LD   V7, 2          ; frames of beep on a bounce

loop:
    LD   I, ball
    DRW  V0, V1, 4      ; draw at the current position
    LD   I, ball
    DRW  V0, V1, 4      ; and erase it again on the next frame

    ADD  V0, V2
    SNE  V0, 61         ; ran off the right edge (64 - 4 + 1)
    CALL flip_x
    SNE  V0, 255        ; wrapped below zero, so it ran off the left
    CALL flip_x

    ADD  V1, V3
    SNE  V1, 29         ; ran off the bottom (32 - 4 + 1)
    CALL flip_y
    SNE  V1, 255        ; ran off the top
    CALL flip_y

    JP   loop

; Negate the velocity by subtracting it from zero, then take one step with the
; new sign, which lands the ball exactly back on the edge it overshot.
flip_x:
    LD   V6, 0
    SUB  V6, V2
    LD   V2, V6
    ADD  V0, V2
    LD   ST, V7
    RET

flip_y:
    LD   V6, 0
    SUB  V6, V3
    LD   V3, V6
    ADD  V1, V3
    LD   ST, V7
    RET

; Four rows of four pixels. Sprites are always eight wide, so only the high
; nibble of each byte is used.
ball:
    DB 0xF0, 0xF0, 0xF0, 0xF0
