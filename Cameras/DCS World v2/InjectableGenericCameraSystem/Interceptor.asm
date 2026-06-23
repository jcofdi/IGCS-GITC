;////////////////////////////////////////////////////////////////////////////////////////////////////////
;// Part of Injectable Generic Camera System
;// Copyright(c) 2020, Frans Bouma
;// All rights reserved.
;// https://github.com/FransBouma/InjectableGenericCameraSystem
;//
;// Redistribution and use in source and binary forms, with or without
;// modification, are permitted provided that the following conditions are met :
;//
;//  * Redistributions of source code must retain the above copyright notice, this
;//	  list of conditions and the following disclaimer.
;//
;//  * Redistributions in binary form must reproduce the above copyright notice,
;//    this list of conditions and the following disclaimer in the documentation
;//    and / or other materials provided with the distribution.
;//
;// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
;// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
;// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
;// DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
;// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
;// DAMAGES(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
;// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
;// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
;// OR TORT(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
;// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
;////////////////////////////////////////////////////////////////////////////////////////////////////////
;---------------------------------------------------------------
; Game specific asm file for DCS World
; Hooks DCS.exe rendering camera (double-precision)
;---------------------------------------------------------------

PUBLIC cameraStructInterceptor
PUBLIC cameraWrite1Interceptor
PUBLIC cameraWrite2Interceptor
PUBLIC cameraFovWriteInterceptor

EXTERN g_cameraEnabled: byte
EXTERN g_cameraStructAddress: qword
EXTERN g_fovRenderAddress: qword
EXTERN g_fovHookCallCount: dword
EXTERN g_customFovDegrees: qword
EXTERN g_renderRot: qword
EXTERN g_activeCameraMat: qword

EXTERN _cameraStructInterceptionContinue: qword
EXTERN _cameraWrite1InterceptionContinue: qword
EXTERN _cameraWrite2InterceptionContinue: qword
EXTERN _cameraFovWriteInterceptionContinue: qword

.data

.code

cameraStructInterceptor PROC
; DCS.exe - Freecam position write block
; Captures RBX as camera struct base AND blocks all 3 position writes.
;
; Original code (32 bytes = 0x20):
;   movsd [rbx+100h], xmm9     ; 9 bytes  - pos X
;   movsd [rbx+108h], xmm6     ; 8 bytes  - pos Y
;   addsd xmm10, [rbp-50h]     ; 6 bytes  - compute pos Z delta
;   movsd [rbx+110h], xmm10    ; 9 bytes  - pos Z

	mov [g_cameraStructAddress], rbx
	cmp byte ptr [g_cameraEnabled], 1
	jne originalCode
	jmp qword ptr [_cameraStructInterceptionContinue]
originalCode:
	movsd qword ptr [rbx+100h], xmm9
	movsd qword ptr [rbx+108h], xmm6
	addsd xmm10, qword ptr [rbp-50h]
	movsd qword ptr [rbx+110h], xmm10
	jmp qword ptr [_cameraStructInterceptionContinue]
cameraStructInterceptor ENDP

cameraWrite1Interceptor PROC
; DCS.exe+659200 - Rotation function A (yaw rotation)
; RCX = rotation matrix pointer (struct_base + 0xA0)
; When IGCS enabled and this is our camera, skip entire function (ret).
;
; Original entry (14 bytes):
;   push rbx                    ; 2 bytes  (40 53)
;   sub rsp, 40h                ; 4 bytes  (48 83 EC 40)
;   movaps [rsp+30h], xmm6     ; 5 bytes  (0F 29 74 24 30)
;   mov rbx, rcx                ; 3 bytes  (48 8B D9)

	; ALL-MODES: capture the active view camera (rcx = cam+0xA0) UNCONDITIONALLY -- whenever DCS
	; sets this camera's rotation, in ANY mode -- so we can find/identify it without visiting F11.
	; Sub-render cameras are Visualizer objects and don't reach here, so this only hits the active
	; gameplay view camera. Storing a pointer has no game side effect; the rotation DRIVE below
	; stays gated on g_cameraEnabled.
	mov [g_activeCameraMat], rcx
	cmp byte ptr [g_cameraEnabled], 1
	jne originalCode
	; Pre-load OUR rotation into [rcx] (+0xA0). matBase layout, stride 0x20 per row.
	push rax
	mov rax, qword ptr [g_renderRot+00h]
	mov qword ptr [rcx+00h], rax
	mov rax, qword ptr [g_renderRot+08h]
	mov qword ptr [rcx+08h], rax
	mov rax, qword ptr [g_renderRot+10h]
	mov qword ptr [rcx+10h], rax
	mov rax, qword ptr [g_renderRot+18h]
	mov qword ptr [rcx+20h], rax
	mov rax, qword ptr [g_renderRot+20h]
	mov qword ptr [rcx+28h], rax
	mov rax, qword ptr [g_renderRot+28h]
	mov qword ptr [rcx+30h], rax
	mov rax, qword ptr [g_renderRot+30h]
	mov qword ptr [rcx+40h], rax
	mov rax, qword ptr [g_renderRot+38h]
	mov qword ptr [rcx+48h], rax
	mov rax, qword ptr [g_renderRot+40h]
	mov qword ptr [rcx+50h], rax
	pop rax
originalCode:
	push rbx
	sub rsp, 40h
	movaps xmmword ptr [rsp+30h], xmm6
	mov rbx, rcx
	jmp qword ptr [_cameraWrite1InterceptionContinue]
skipFunction:
	ret
cameraWrite1Interceptor ENDP

cameraWrite2Interceptor PROC
; DCS.exe+659050 - Rotation function B (pitch rotation)
; RCX = rotation matrix pointer (struct_base + 0xA0)
; When IGCS enabled and this is our camera, skip entire function (ret).
;
; Original entry (14 bytes):
;   push rbx                    ; 2 bytes  (40 53)
;   sub rsp, 40h                ; 4 bytes  (48 83 EC 40)
;   movaps [rsp+30h], xmm6     ; 5 bytes  (0F 29 74 24 30)
;   movaps xmm0, xmm1          ; 3 bytes  (0F 28 C1)

	; ALL-MODES: capture the active view camera (rcx = cam+0xA0) UNCONDITIONALLY (see WRITE1).
	mov [g_activeCameraMat], rcx
	cmp byte ptr [g_cameraEnabled], 1
	jne originalCode
	push rax
	mov rax, qword ptr [g_renderRot+00h]
	mov qword ptr [rcx+00h], rax
	mov rax, qword ptr [g_renderRot+08h]
	mov qword ptr [rcx+08h], rax
	mov rax, qword ptr [g_renderRot+10h]
	mov qword ptr [rcx+10h], rax
	mov rax, qword ptr [g_renderRot+18h]
	mov qword ptr [rcx+20h], rax
	mov rax, qword ptr [g_renderRot+20h]
	mov qword ptr [rcx+28h], rax
	mov rax, qword ptr [g_renderRot+28h]
	mov qword ptr [rcx+30h], rax
	mov rax, qword ptr [g_renderRot+30h]
	mov qword ptr [rcx+40h], rax
	mov rax, qword ptr [g_renderRot+38h]
	mov qword ptr [rcx+48h], rax
	mov rax, qword ptr [g_renderRot+40h]
	mov qword ptr [rcx+50h], rax
	pop rax
originalCode:
	push rbx
	sub rsp, 40h
	movaps xmmword ptr [rsp+30h], xmm6
	movaps xmm0, xmm1
	jmp qword ptr [_cameraWrite2InterceptionContinue]
skipFunction:
	ret
cameraWrite2Interceptor ENDP

cameraFovWriteInterceptor PROC
; DCS.exe+579B94 area - Rendering FOV clamp+write (runs every frame)
; Captures RAX as the render FOV pointer. When camera enabled AND this is
; our camera's FOV, writes our custom FOV (degrees) bypassing clamp.
;
; Original code (15 bytes = 0x0F):
;   maxsd xmm0,[rax]           ; 4 bytes - clamp min
;   minsd xmm0,xmm1            ; 4 bytes - clamp max
;   movsd [rax],xmm0           ; 4 bytes - write clamped FOV
;   mov rax,[rdi]               ; 3 bytes - load next pointer

	lock inc dword ptr [g_fovHookCallCount]

	cmp byte ptr [g_cameraEnabled], 1
	jne originalCode

	push rbx
	mov rbx, [g_cameraStructAddress]
	add rbx, 08h
	cmp rax, rbx
	pop rbx
	jne originalCode

	mov [g_fovRenderAddress], rax
	movsd xmm0, qword ptr [g_customFovDegrees]
	movsd qword ptr [rax], xmm0
	mov rax, [rdi]
	jmp qword ptr [_cameraFovWriteInterceptionContinue]

originalCode:
	maxsd xmm0, qword ptr [rax]
	minsd xmm0, xmm1
	movsd qword ptr [rax], xmm0
	mov rax, [rdi]
	jmp qword ptr [_cameraFovWriteInterceptionContinue]
cameraFovWriteInterceptor ENDP

END
