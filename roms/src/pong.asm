; pong.asm - two players, first to nine.
;
; Left player is 1 and Q (keypad 1 and 4), right player is 4 and R (keypad C
; and D). The scores sit at the top and are redrawn only when they change.
;
; The paddles are vertical, so unlike the other ROMs here the hit test runs on
; y rather than x: a paddle is one pixel wide and six tall, and the ball counts
; as returned when its row is within those six of the paddle's top. Testing at
; x = 2 and x = 61 rather than at the paddle's own column means the check
; happens one step before the ball would reach the wall behind it, which is
; what leaves room to tell a return apart from a point.
;
;   V0 ball x        V4 left paddle y    V8 scratch      VC scratch
;   V1 ball y        V5 right paddle y   V9 scratch      VD left column (1)
;   V2 x velocity    V6 scratch          VA left score   VE right column (62)
;   V3 y velocity    V7 scratch          VB right score  VF collision (system)

start:
    LD   VA, 0          ; scores
    LD   VB, 0

new_rally:
    CLS
    LD   VD, 1          ; the two paddle columns, in registers because DRW
    LD   VE, 62         ; takes both coordinates as registers
    LD   V4, 13         ; paddles start centred
    LD   V5, 13
    LD   I, paddle
    DRW  VD, V4, 6
    LD   I, paddle
    DRW  VE, V5, 6
    CALL draw_scores

    LD   V0, 32         ; ball starts in the middle, heading left
    LD   V1, 16
    LD   V2, 255
    LD   V3, 1
    LD   I, ball
    DRW  V0, V1, 1      ; drawn here so the first erase in the loop has
                        ; something to erase rather than switching a pixel on

rally:
    CALL wait_tick
    CALL move_paddles

    LD   I, ball
    DRW  V0, V1, 1      ; erase

    ADD  V1, V3         ; vertical first: the walls are unconditional
    SNE  V1, 255        ; off the top
    CALL flip_y
    SNE  V1, 32         ; off the bottom
    CALL flip_y

    ADD  V0, V2
    SNE  V0, 2          ; level with the left paddle
    CALL left_paddle
    SNE  V0, 61         ; level with the right paddle
    CALL right_paddle
    SNE  V0, 255        ; past the left wall, so the right player scores
    JP   right_scores
    SNE  V0, 64         ; past the right wall
    JP   left_scores

    LD   I, ball
    DRW  V0, V1, 1
    JP   rally

; ---------------------------------------------------------------------------
; Ball
; ---------------------------------------------------------------------------

flip_y:
    LD   V6, 0
    SUB  V6, V3
    LD   V3, V6
    ADD  V1, V3
    RET

flip_x:
    LD   V6, 0
    SUB  V6, V2
    LD   V2, V6
    ADD  V0, V2
    RET

; Returned if paddle_y <= ball_y < paddle_y + 6. A miss just falls through and
; the ball carries on into the wall, where the scoring check picks it up.
left_paddle:
    LD   V6, V1
    SUB  V6, V4         ; VF = 1 when the ball is at or below the paddle top
    SE   VF, 1
    RET
    LD   V7, V6
    LD   V9, 6
    SUB  V7, V9         ; VF = 0 when the offset is under 6, so a return
    SE   VF, 0
    RET
    CALL flip_x
    RET

right_paddle:
    LD   V6, V1
    SUB  V6, V5
    SE   VF, 1
    RET
    LD   V7, V6
    LD   V9, 6
    SUB  V7, V9
    SE   VF, 0
    RET
    CALL flip_x
    RET

; ---------------------------------------------------------------------------
; Paddles
; ---------------------------------------------------------------------------

move_paddles:
    LD   V6, 1          ; keypad 1 is physical 1
    SKNP V6
    CALL left_up
    LD   V6, 4          ; keypad 4 is physical Q
    SKNP V6
    CALL left_down
    LD   V6, 0x0C       ; keypad C is physical 4
    SKNP V6
    CALL right_up
    LD   V6, 0x0D       ; keypad D is physical R
    SKNP V6
    CALL right_down
    RET

; Paddles are six tall on a 32-row screen, so 26 is the lowest top edge. They
; start on 13 and move two at a time, which lands on 1 and 25 rather than 0 and
; 26 - close enough to the edges, and it keeps the bounds test to one compare.
left_up:
    SNE  V4, 1
    RET
    LD   I, paddle
    DRW  VD, V4, 6
    ADD  V4, 254
    LD   I, paddle
    DRW  VD, V4, 6
    RET

left_down:
    SNE  V4, 25
    RET
    LD   I, paddle
    DRW  VD, V4, 6
    ADD  V4, 2
    LD   I, paddle
    DRW  VD, V4, 6
    RET

right_up:
    SNE  V5, 1
    RET
    LD   I, paddle
    DRW  VE, V5, 6
    ADD  V5, 254
    LD   I, paddle
    DRW  VE, V5, 6
    RET

right_down:
    SNE  V5, 25
    RET
    LD   I, paddle
    DRW  VE, V5, 6
    ADD  V5, 2
    LD   I, paddle
    DRW  VE, V5, 6
    RET

; ---------------------------------------------------------------------------
; Scoring
; ---------------------------------------------------------------------------

left_scores:
    ADD  VA, 1
    SNE  VA, 9
    JP   start          ; nine wins it, so start a fresh match
    JP   new_rally

right_scores:
    ADD  VB, 1
    SNE  VB, 9
    JP   start
    JP   new_rally

; Both scores stay under ten, so a single font glyph each is enough and there
; is no BCD conversion to do.
draw_scores:
    LD   V6, 26
    LD   V7, 1
    LD   F, VA
    DRW  V6, V7, 5
    LD   V6, 34
    LD   F, VB
    DRW  V6, V7, 5
    RET

wait_tick:
    LD   V6, 2
    LD   DT, V6
tick_wait:
    LD   V6, DT
    SE   V6, 0
    JP   tick_wait
    RET

ball:
    DB 0x80
paddle:
    DB 0x80, 0x80, 0x80, 0x80, 0x80, 0x80
