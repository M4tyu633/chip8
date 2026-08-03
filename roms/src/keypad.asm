; keypad.asm - press a key, see its hex digit.
;
; A test for the two halves of input handling that are easy to get wrong:
; Fx0A, which must block until a key is pressed *and released* rather than
; completing on the press, and Fx29, which points I at the built-in font glyph
; for a digit.
;
; The 16 keys map to the original COSMAC VIP keypad:
;
;     1 2 3 C          on a QWERTY keyboard:      1 2 3 4
;     4 5 6 D                                     Q W E R
;     7 8 9 E                                     A S D F
;     A 0 B F                                     Z X C V

start:
    CLS
    LD   V3, 30         ; glyph position, roughly centred
    LD   V4, 13
    LD   V5, 2          ; frames of beep per press

loop:
    LD   V0, K          ; blocks here until a key is pressed and released
    LD   ST, V5
    CLS
    LD   F, V0          ; I now points at the glyph for the key's value
    DRW  V3, V4, 5
    JP   loop
