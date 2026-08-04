; brix.asm - a brick-breaker. The one bundled ROM you can actually play.
;
; Three rows of eight bricks, a paddle, three lives, and a score shown on the
; game-over screen. Controls are A and D (keypad 7 and 9).
;
; Two things here are worth explaining, because both look like detours and
; neither is.
;
; Erasing a brick. A hit is detected with the collision flag VF sets on DRW,
; but that flag only says "a lit pixel was switched off" - and the pixel it
; switched off is one pixel of the brick, which leaves a hole rather than
; removing anything. So on a collision the ball is drawn a second time first,
; which XORs it away and restores the brick to whole, and only then is the
; brick's own sprite XORed off. Erasing the brick without undoing the ball
; first leaves a single stuck pixel behind, which is the bug this ordering
; exists to avoid.
;
; Finding the column. Dividing the ball's x by 8 would be one shift, but SHR
; is the instruction the shift quirk disagrees about: on the COSMAC it reads
; its second operand and on the CHIP-48 it reads its first, and this ROM has
; to survive either setting of F1-F5 in the debugger. Walking up in steps of
; eight instead is a few more instructions and means the same thing on every
; interpreter.
;
;   V0 ball x        V4 paddle x       V8 lives          VC score
;   V1 ball y        V5 scratch        V9 scratch        VD paddle row (31)
;   V2 x velocity    V6 scratch        VA brick x        VE ball-lost flag
;   V3 y velocity    V7 bricks left    VB brick y        VF collision (system)

start:
    LD   V8, 3          ; lives
    LD   VC, 0          ; score
    LD   VD, 31         ; the paddle's row, kept in a register because DRW
                        ; takes its coordinates as registers and nothing else

new_board:
    CLS
    CALL draw_bricks
    LD   V7, 24         ; 3 rows x 8 columns

; Put the paddle back under the centre and drop the ball onto it. The serve
; deliberately starts two pixels above the paddle and moving down, so the
; first bounce is free and the player gets the whole climb to the bricks to
; find the keys.
serve:
    LD   V4, 28         ; paddle x, even, and every move is +/-2, so it lands
                        ; exactly on 0 and 56 rather than overshooting
    LD   I, paddle
    DRW  V4, VD, 1
    LD   V0, 31         ; ball x
    LD   V1, 29         ; ball y
    LD   V2, 1          ; moving right
    LD   V3, 1          ; moving down, onto the paddle
    LD   I, ball
    DRW  V0, V1, 1      ; draw it here, because move_ball opens by erasing the
                        ; previous position: with nothing on screen to erase
                        ; that first XOR would switch a pixel *on* and leave it
                        ; stuck there for the rest of the game

; The paddle is polled twice for every step the ball takes. Moving both on the
; same tick is the obvious loop and it plays badly: slowing the ball to
; something you can react to slows the paddle with it, and a paddle that
; crawls is worse than a fast ball. Two waits per iteration decouples them.
game_loop:
    CALL wait_tick
    CALL move_paddle
    CALL wait_tick
    CALL move_paddle
    CALL move_ball
    SE   VE, 0
    JP   life_lost
    SNE  V7, 0
    JP   level_clear
    JP   game_loop

; ---------------------------------------------------------------------------
; Timing
; ---------------------------------------------------------------------------

; One tick every two frames. The game loop waits twice per iteration, so the
; ball moves a pixel every four frames - 15 a second - while the paddle is
; polled every two and still travels 60. Timed off the delay timer rather than
; the display-wait quirk, because that quirk is one of the ones F1-F5 can
; switch off and the game should not change speed when it does.
wait_tick:
    LD   V5, 2
    LD   DT, V5
tick_wait:
    LD   V5, DT
    SE   V5, 0
    JP   tick_wait
    RET

pause:
    LD   V5, 45
    LD   DT, V5
pause_wait:
    LD   V5, DT
    SE   V5, 0
    JP   pause_wait
    RET

; ---------------------------------------------------------------------------
; The board
; ---------------------------------------------------------------------------

; Rows two pixels tall at y = 2, 5 and 8, columns every 8 pixels. The sprite is
; seven wide with a blank eighth column, so neighbouring bricks stay visibly
; separate without any gap arithmetic.
draw_bricks:
    LD   VB, 2          ; row y
    LD   V9, 3          ; rows left
brick_row:
    LD   VA, 0          ; column x
    LD   V5, 8          ; columns left
brick_col:
    LD   I, brick
    DRW  VA, VB, 2
    ADD  VA, 8
    ADD  V5, 255        ; subtract one
    SNE  V5, 0
    JP   brick_next_row
    JP   brick_col
brick_next_row:
    ADD  VB, 3
    ADD  V9, 255
    SNE  V9, 0
    RET
    JP   brick_row

; ---------------------------------------------------------------------------
; Paddle
; ---------------------------------------------------------------------------

move_paddle:
    LD   V5, 7          ; keypad 7 is physical A
    SKNP V5
    CALL paddle_left
    LD   V5, 9          ; keypad 9 is physical D
    SKNP V5
    CALL paddle_right
    RET

paddle_left:
    SNE  V4, 0
    RET                 ; already against the left wall
    LD   I, paddle
    DRW  V4, VD, 1      ; erase
    ADD  V4, 254        ; -2
    LD   I, paddle
    DRW  V4, VD, 1      ; and redraw two pixels over
    RET

paddle_right:
    SNE  V4, 56         ; 56 + 7 is the last column, so this is the far wall
    RET
    LD   I, paddle
    DRW  V4, VD, 1
    ADD  V4, 2
    LD   I, paddle
    DRW  V4, VD, 1
    RET

; ---------------------------------------------------------------------------
; Ball
; ---------------------------------------------------------------------------

move_ball:
    LD   I, ball
    DRW  V0, V1, 1      ; erase where it was

    ADD  V0, V2
    SNE  V0, 64         ; off the right edge
    CALL flip_x
    SNE  V0, 255        ; wrapped below zero, so off the left
    CALL flip_x

    LD   VE, 0          ; assume the ball is still in play
    ADD  V1, V3
    SNE  V1, 255        ; off the top
    CALL flip_y
    SNE  V1, 31         ; down on the paddle's row
    CALL paddle_row
    SE   VE, 0
    RET                 ; lost, so leave it undrawn and let game_loop see VE

    LD   I, ball
    DRW  V0, V1, 1      ; draw where it is now
    SE   VF, 1
    RET                 ; hit nothing

    LD   I, ball
    DRW  V0, V1, 1      ; undo, restoring the brick to a whole sprite
    CALL hit_brick
    LD   I, ball
    DRW  V0, V1, 1      ; and put the ball back
    RET

; Negate a velocity by subtracting it from zero, then take one step with the
; new sign, which lands the ball back on the edge it just overshot.
flip_x:
    LD   V5, 0
    SUB  V5, V2
    LD   V2, V5
    ADD  V0, V2
    RET

flip_y:
    LD   V5, 0
    SUB  V5, V3
    LD   V3, V5
    ADD  V1, V3
    RET

; Reached row 31. Caught if paddle_x <= ball_x < paddle_x + 8. CHIP-8 has no
; signed compare, so both halves are read off the borrow flag SUB leaves in VF.
paddle_row:
    LD   V5, V0
    SUB  V5, V4         ; VF = 1 when the ball is at or right of the paddle
    SE   VF, 1
    JP   ball_missed
    LD   V6, V5         ; V5 is now the offset along the paddle
    LD   V9, 8
    SUB  V6, V9         ; VF = 0 when that offset is under 8, so a hit
    SE   VF, 0
    JP   ball_missed
    LD   V5, 0          ; caught: send it back up
    SUB  V5, V3
    LD   V3, V5
    ADD  V1, V3
    RET
ball_missed:
    LD   VE, 1
    RET

; ---------------------------------------------------------------------------
; Bricks
; ---------------------------------------------------------------------------

; Work out which brick the ball is standing in, clear it, and bounce. Only the
; vertical velocity flips: a real brick breaker would reflect a side-on hit
; horizontally, but telling a side hit from a face hit needs the approach
; direction, and at one pixel a tick the difference is not visible anyway.
hit_brick:
    LD   VB, 255        ; 255 means "not in a brick row after all"
    SNE  V1, 2
    LD   VB, 2
    SNE  V1, 3
    LD   VB, 2
    SNE  V1, 5
    LD   VB, 5
    SNE  V1, 6
    LD   VB, 5
    SNE  V1, 8
    LD   VB, 8
    SNE  V1, 9
    LD   VB, 8
    SNE  VB, 255
    JP   brick_bounce   ; hit something that is not a brick, so just bounce

    LD   VA, 0          ; walk up in eights to the column the ball is in
find_col:
    LD   V5, VA
    ADD  V5, 8
    LD   V6, V0
    SUB  V6, V5         ; VF = 1 while the ball is past the next boundary
    SE   VF, 1
    JP   clear_brick
    ADD  VA, 8
    JP   find_col

clear_brick:
    LD   I, brick
    DRW  VA, VB, 2      ; XOR the whole brick away
    ADD  VC, 1          ; one point
    ADD  V7, 255        ; one fewer brick on the board

brick_bounce:
    LD   V5, 0
    SUB  V5, V3
    LD   V3, V5
    RET

; ---------------------------------------------------------------------------
; Winning, losing, and the score
; ---------------------------------------------------------------------------

life_lost:
    LD   I, paddle
    DRW  V4, VD, 1      ; take the paddle off before the pause
    ADD  V8, 255
    SNE  V8, 0
    JP   game_over
    CALL pause
    JP   serve          ; same board, the surviving bricks stay put

level_clear:
    CALL pause
    JP   new_board      ; fresh bricks, same lives and score

; Score tops out at 24 a board, so only the last two digits are worth drawing.
; LD B writes the three BCD digits to memory at I, and LD V2, [I] reads them
; back into V0-V2 - which clobbers the ball's position, but the game is over
; and `start` sets all of it again.
game_over:
    CLS
    LD   I, scratch
    LD   B, VC
    LD   I, scratch
    LD   V2, [I]
    LD   V5, 26
    LD   V6, 13
    LD   F, V1          ; tens
    DRW  V5, V6, 5
    ADD  V5, 6
    LD   F, V2          ; ones
    DRW  V5, V6, 5
wait_key:
    LD   V0, K          ; any key starts a new game
    JP   start

; ---------------------------------------------------------------------------
; Sprite data
; ---------------------------------------------------------------------------

; Sprites are always eight pixels wide, so a one-pixel ball is the top bit
; alone and the paddle is a full byte.
ball:
    DB 0x80
paddle:
    DB 0xFF
brick:
    DB 0xFE, 0xFE       ; seven lit, one blank, so bricks do not merge
scratch:
    DB 0, 0, 0          ; three bytes for LD B to write the score into
