; catch.asm - catch the falling blocks in a bucket. Three misses and it ends.
;
; The smallest thing that is still a game, and the one to read first if you
; want to see how the others are put together: a paddle, one moving object, a
; hit test, and a score.
;
; The drop position is `RND V0, 0x38`, which is worth a second look. RND masks
; its random byte, so masking with 0x38 keeps only bits 3, 4 and 5 and yields
; one of 0, 8, 16 ... 56 - a random column already aligned to eight pixels and
; already inside the screen, with no clamping and no rejection loop.
;
;   V0 block x       V4 bucket x      V8 lives         VC score
;   V1 block y       V5 scratch       V9 scratch       VD bucket row (31)
;                    V6 scratch                        VF collision (system)

start:
    LD   VD, 31         ; the bucket's row, kept in a register for DRW
    LD   V8, 3          ; lives
    LD   VC, 0          ; score
    CLS
    LD   V4, 28         ; bucket x, even, and it moves +/-2, so it lands
                        ; exactly on 0 and 56 rather than stepping past them
    LD   I, bucket
    DRW  V4, VD, 1

next_block:
    RND  V0, 0x38       ; a random column, already a multiple of eight
    LD   V1, 0
    LD   I, block
    DRW  V0, V1, 2

; Two waits per row the block falls, so the bucket is polled twice as often as
; the block moves. A block slow enough to react to and a bucket tied to the
; same tick would be a bucket you cannot get across the screen in time.
fall:
    CALL wait_tick
    CALL move_bucket
    CALL wait_tick
    CALL move_bucket
    LD   I, block
    DRW  V0, V1, 2      ; erase
    ADD  V1, 1
    SNE  V1, 30         ; the block is two tall, so 30 is the bucket's lip
    JP   landed
    LD   I, block
    DRW  V0, V1, 2      ; and redraw one row down
    JP   fall

; Caught if bucket_x <= block_x < bucket_x + 8. As everywhere else here, the
; comparison is read off the borrow flag SUB leaves in VF, because CHIP-8 has
; no signed compare.
landed:
    LD   V5, V0
    SUB  V5, V4         ; VF = 1 when the block is at or right of the bucket
    SE   VF, 1
    JP   dropped
    LD   V6, V5
    LD   V9, 8
    SUB  V6, V9         ; VF = 0 when the offset is under 8, so a catch
    SE   VF, 0
    JP   dropped
    ADD  VC, 1
    JP   next_block

dropped:
    ADD  V8, 255
    SNE  V8, 0
    JP   game_over
    JP   next_block

; One tick every three frames. The bucket redraws twice per move and the block
; twice per fall, and display_wait caps drawing at one per frame, so the tick
; has to be long enough that those draws are not what sets the pace.
wait_tick:
    LD   V5, 3
    LD   DT, V5
tick_wait:
    LD   V5, DT
    SE   V5, 0
    JP   tick_wait
    RET

move_bucket:
    LD   V5, 7          ; keypad 7 is physical A
    SKNP V5
    CALL bucket_left
    LD   V5, 9          ; keypad 9 is physical D
    SKNP V5
    CALL bucket_right
    RET

bucket_left:
    SNE  V4, 0
    RET
    LD   I, bucket
    DRW  V4, VD, 1
    ADD  V4, 254        ; -2
    LD   I, bucket
    DRW  V4, VD, 1
    RET

bucket_right:
    SNE  V4, 56         ; 56 + 7 is the last column
    RET
    LD   I, bucket
    DRW  V4, VD, 1
    ADD  V4, 2
    LD   I, bucket
    DRW  V4, VD, 1
    RET

; LD B writes the score's three BCD digits at I and LD V2, [I] reads them into
; V0-V2, clobbering the block position - which is fine, `start` sets it again.
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
    LD   V0, K
    JP   start

block:
    DB 0xC0, 0xC0       ; 2 x 2
bucket:
    DB 0xFF             ; 8 wide, sprites are always eight across
scratch:
    DB 0, 0, 0
