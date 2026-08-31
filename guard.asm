; Landing pad for the spawn trampoline, for MSVC (ml64).
; Byte-for-byte the same sequence as the GNU asm block in
; guard.c; keep the two in sync.
_TEXT SEGMENT

PUBLIC ShGuardAlign

ALIGN 16
ShGuardAlign PROC
    ; The stale return arrives with a dead spawner in rcx,
    ; so bail out instead of running on.
    DB 48H, 85H, 0C9H          ; test rcx, rcx
    DB 75H, 01H                ; jne  +1
    DB 0C3H                    ; ret

    ; Pad out the rest of the slot. Widths are pinned so a
    ; rebuild cannot re-pack them and shrink the pad.
    DB 0FH, 1FH, 44H, 00H, 00H    ; 5-byte nop
    DB 0FH, 1FH, 40H, 00H         ; 4-byte nop
    DB 0FH, 1FH, 80H, 00H, 00H, 00H, 00H  ; 7-byte nop
    DB 0FH, 1FH, 00H              ; 3-byte nop
    DB 66H, 0FH, 1FH, 44H, 00H, 00H   ; 6-byte nop
    DB 66H, 90H                   ; 2-byte nop
    DB 0FH, 1FH, 44H, 00H, 00H    ; 5-byte nop
    DB 0FH, 1FH, 40H, 00H         ; 4-byte nop
    DB 66H, 0FH, 1FH, 44H, 00H, 00H   ; 6-byte nop
    DB 0FH, 1FH, 00H              ; 3-byte nop

    DB 0C3H                       ; ret
ShGuardAlign ENDP

_TEXT ENDS
END
