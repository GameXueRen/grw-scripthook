# Direct chat send (RESEARCH, SHELVED 2026-09-08)

Goal: send chat text by calling the engine's network API directly,
bypassing the chat UI field entirely.  Motivation (BUG7): injected
per-character input into the native chat field truncates long texts
(~9 chars survive, regardless of channel/pace).  Native typed input
is fine, native Ctrl+V paste is NOT supported (tested), so simulation
cannot be fixed - only this API can.

Status: **shelved after 5 recon rounds.**  The send path was NOT
found.  This file preserves everything learned so the work can
resume.  `scripthook_chatapi.c` (dormant, NOT in the build) holds the
recon/hook tooling; re-add it to Makefile + build_msvc.ps1 +
loader.c (`ShChatApiStartup`) to continue.

## Facts established (GRW.exe, build ~2026-08, 2026-09-07/08)

PE layout (Ubisoft-repurposed section names):
- `.xtext` RAW 0x0389B200 VA 0x0389C000 - registry names + jmp thunks
- `.link`  RAW 0x04F4DC00 VA 0x0521E000 - packed bodies (~324MB)
- Part of the engine code region (RVA 0xB37xxx..0xE4xxxx) is UNPACKED
  at runtime; e.g. RVA 0xB3B9B0 is real code.

Network message registry (in .xtext, entries are inline name strings
followed by groups of ~13 function pointers):
- "SilexNetMessageChatMessage.Broadcast" name @ RVA 0x0394A700
- "SilexNetMessageChatMessage.Unicast"   name @ RVA 0x0394A778
- AT RUNTIME the strings are consumed; those RVAs hold the pointer
  groups themselves.  Unicast group (13 ptrs, round-3 dump):
  b3b9b0 b42610 b48510 b48bd0 b488a0 b37620 b37ba0 b3bbc0 b3c1d0
  b39a70 b3a260 b381a0 b38f20 (RVA order).
- Broadcast group first six: e446d0 e28fc0 e27070 e3e900 e32340
  e3ffb0 (all still-packed jmp thunks).
- NetChatController::* strings are VOIP (voice), unrelated.

Dead ends (do not retry):
- RTTI: no "ChatMessage" ASCII anywhere in the image at runtime.
- reflection_classes.tsv: crc32("SilexNetMessageChatMessage") is not
  in the self_hash column - class-hash algo differs from the method
  crc32.  Method names ARE crc32 (0x1900FE56 Broadcast, 0x79A10492
  Unicast) once an object with a method table is found.
- RVA 0xB37620: a single jmp thunk, NOT a vtable.  Referenced by two
  persistent engine-heap objects (0x800104fd18 / +0x38 in one
  session; addresses move per session - pattern: vtable-like +0x00,
  +0x18=5, +0x58 tick counter).
- A 200ms whole-heap watch for instances never caught a transient
  message object.

## Round 4/5: MinHook on the registry entries (12 hooks, all armed)

Results with the player sending native chat (test001/test002):
- bcast.t0 + bcast.t1 fire in PAIRS: args (X, X, 0, 0x7ff7669d2be8)
  then (X, X, 0x7ff766a09688 = RVA 0x394E688, junk=(X>>16)&0xffff).
  BUT: 2 messages produced 6+ pairs at 15ms (frame) cadence with a
  FRESH `this` each - periodic tick traffic, not the chat send.
- uni.t0..t5 fire constantly (ambient network churn).
- The `this` objects carry no text: dumped 0x100 bytes + followed
  every heap pointer (0x60 each) - "test001" appears NOWHERE in any
  dump, ASCII or UTF-16.
- Object head: +0x00 -> RVA 0x394A7F8 (registry metadata), +0x10 ->
  0x3AAA170, +0x30/+0x38 -> self-referential heap node.

Conclusion: the actual chat send happens through a different path
(maybe one of the ~14 unhooked registry entries, maybe the text
travels through an engine-string handle, maybe via a UI-side
serialize before bcast sees an already-empty object).

## Tooling that works (kept in scripthook_chatapi.c)

- MinHook on engine thunks at fixed RVAs: 12/12 armed, zero crashes,
  trivially extensible (`g_slots` table + DEFINE_HOOK macro).
- ShReadMem-based whole-image byte scan (works, ~1s).
- ReadProcessMemory whole-heap qword scan (works, engine heaps =
  big RW regions >= 0x10000, base 0x1_00000000..0x800_00000000).
- Hook-log pattern: log args + DumpThis (hex+ascii, pointer chase),
  chain to trampoline.  See round-4 code.

## Resume plan

1. Hook ALL ~26 registry entries (the 14 not yet hooked: bcast group
   tail + b37ba0 b3bbc0 b3c1d0 b39a70 b3a260 b381a0 b38f20).
2. On every hit, background-scan the heap for the just-sent text
   (ASCII + UTF-16 needle); whoever's `this` leads to it (directly or
   one hop) is the real send path.
3. Alternative: come in from the UI side - find the native chat
   input widget (it holds the typed text; the game reads DirectInput
   but the field is an engine text widget), trace its submit handler.
4. Once found: construct/clone the message, set text, call Broadcast
   via ShQueueCall on the game thread.  cnchat's HandleDone swaps its
   PostMessage loop for that call (fallback stays).
