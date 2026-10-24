; The native prototype is unreviewed: preserve the entry machine state, not a guessed C++ call.
option casemap:none
EXTERN VoiceProbeObserve:PROC
EXTERN VoiceProbeTrampoline:QWORD
EXTERN VoiceProbeUseXsave:DWORD
EXTERN VoiceProbeMaskLo:DWORD
EXTERN VoiceProbeMaskHi:DWORD
EXTERN __chkstk:PROC
EXTERN __imp_GetLastError:QWORD
EXTERN __imp_SetLastError:QWORD
PUBLIC VoiceProbeEntry
.code
VoiceProbeEntry PROC FRAME
    pushfq
    .allocstack 8
    push rax
    .allocstack 8
    push rcx
    .allocstack 8
    push rdx
    .allocstack 8
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r8
    .allocstack 8
    push r9
    .allocstack 8
    push r10
    .allocstack 8
    push r11
    .allocstack 8
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    mov eax,4288h
    call __chkstk
    sub rsp,rax
    .allocstack 4288h
    .endprolog

    ; XSAVE needs 64-byte alignment and a zeroed header/reserved region.
    lea rbx,[rsp+0BFh]
    and rbx,-64
    mov [rsp+40h],rbx
    mov rdi,rbx
    xor eax,eax
    mov ecx,800h
    cld
    rep stosq
    cmp DWORD PTR [VoiceProbeUseXsave],0
    je SaveLegacy
    mov eax,DWORD PTR [VoiceProbeMaskLo]
    mov edx,DWORD PTR [VoiceProbeMaskHi]
    xsave64 [rbx]
    jmp StateSaved
SaveLegacy:
    fxsave64 [rbx]
StateSaved:
    call QWORD PTR [__imp_GetLastError]
    mov DWORD PTR [rsp+48h],eax
    lea rcx,[rsp+4288h]
    call VoiceProbeObserve
    mov ecx,DWORD PTR [rsp+48h]
    call QWORD PTR [__imp_SetLastError]
    mov rbx,[rsp+40h]
    cmp DWORD PTR [VoiceProbeUseXsave],0
    je RestoreLegacy
    mov eax,DWORD PTR [VoiceProbeMaskLo]
    mov edx,DWORD PTR [VoiceProbeMaskHi]
    xrstor64 [rbx]
    jmp StateRestored
RestoreLegacy:
    fxrstor64 [rbx]
StateRestored:
    add rsp,4288h
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    popfq
    ; No extra return address, no guessed fifth/sixth parameter, no post-call code.
    jmp QWORD PTR [VoiceProbeTrampoline]
VoiceProbeEntry ENDP
END
