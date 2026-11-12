# Mission scripts (work in progress)

## Files
- `Scripting/*.prj` (text): `Source_Files { "x.c" ... } Include_Files {...} Definess {} Settings { Debug Source_Path ".." Target_Path ".." }`.
  Parsed by `FUN_100c7450` (scripting project loader, keyword table at 0x1015b7c8).
- `Scripting/*.o`: compiled C-subset bytecode, loaded by `FUN_100c5ef0`.

## .o layout (`FUN_100c5ef0`)
100-byte header = 25 × u32 `h[0..24]`, then a blob of `h[2]` bytes (file size = 100 + h[2]).
| field | use (offsets are into the blob) |
|---|---|
| h[0] | version (1) |
| h[2] | blob size |
| h[3] | stored at script+0x10 (entry / code size, e.g. 0x233) |
| h[5] | script+0x154 |
| h[7] | data (globals) size, copied into a separate block |
| h[9], h[10] | count / offset of a 4-byte table (script+0x374/0x378) |
| h[11], h[12] | count / offset (script+0x364/0x368) |
| h[13], h[14] | count / offset (script+0x34/0x38) |
| h[15], h[16] | count / offset (script+0x36c/0x370) |
| h[17], h[18] | exported variables: count / offset of `{char* name; u16 type; u32 size}` (looked up by `FUN_100cba80`) |
| h[19], h[20] | string/symbol table: count / offset; each entry resolved to a u16 id by `FUN_100c7d20` (source file name, `_-_-_-_-_-_-_-N` labels, include files, builtin names, script symbols, string literals) |
Code begins at blob offset 0 as 32-bit words (e.g. `0x94 line`, `0x20 type value`, `0x24 offset`, ...).

## Builtins
220 engine functions in a table at 0x10160ad0, records of 28 bytes `{char* name; fnptr; u32 class; ...}`
(registered by `FUN_100d9120`, constants such as `CAMERA_CONTROL`, `TACTIC`, `UNIT_INFO` from the table at 0x10268f00).
Builtins take a pointer to the top of the script stack (arguments at negative offsets), e.g. `FUN_100d91b0`.
Overloads share a name (e.g. 8 × `Teleport`). Full list: hg_sin hg_cos hg_tan hg_atan hg_rand Vector*,
SetWaterLevel SetWeather*, BeginFade, Enable/DisableState, World/Mission/Team/UnitSignal, GetTimeStep, Terminate,
Kill, IsAlive, MissionOver, Teleport, GetUnitPosition/Distance, SetObjectiveStatus, Defcon, weapons, dialog,
FollowPath/PausePath/AssignPath, AnimStart/Stop, FlyTo, ControlCamera, cinematic control, Auto* (pilot AI),
checkpoints/objectives, tactics, LookAt/CanTarget, timers, Order/OrderParam/ActivateOrder, speed/rotation,
Attack/Defend/ArtilleryStrike, squads, lights/FX, RegisterString.

## Execution model (solved)
`FUN_100c6700` links the code into threaded code: every instruction word is replaced by its handler pointer,
chosen from the descriptor list for the opcode (`0x101608b8[op]` → `{variant, operandFixup, handler}` triples,
terminated by variant 2). `FUN_100c6ac0` runs a script: `while (pc) pc = handler(pc + 1)`, at most 20000 steps
per tick; an unfinished handler resumes next tick.

Instruction word: `op = (w >> 2) & 0x7f`, `variant = w & 3` (0 = immediate operand, 1 = memory operand).
Instructions are byte-packed (op 6 has a 1-byte operand).

Operands:
- immediate: `u32 type; payload[size(type)]` — sizes `{0, 4, 4, 12, 4, 4}` for void/int/float/vector/string/handle,
  struct types (>5) from the h[17] table. String literals are local string-table indices remapped to global ids.
- address (8 bytes): `w0 = space<<28 | type<<16 | index`, `w1 = byte offset`. Spaces: 0 extern variable (h[15] table,
  resolved by name against exported globals), 1 script data (h[7] bytes), 2 static block, 3 frame (`fp + off - index`),
  4 stack top (`sp + off - index`), 5 register `index` (16 typed registers, r0 = return value).
- binary ops: `u8 flags` (low nibble dest register, 0x10 = write in place to the first operand, 0x20 = immediate is
  the left operand), then address + immediate (variant 0) or address + address (variant 1). Result type = type of the
  first address operand; the first value is converted to it, the second is taken as-is.

| op | name | operands | semantics |
|---|---|---|---|
| 0, 7 | nop | – | |
| 1 | call | rel32 | push return, push fp, fp = frame; jump |
| 2 | callsym | sym | script function by symbol (local or exported by another script); natives become op 3 at link |
| 3 | callnative | sym | `sp += pending`, `fn(stackTop, &r0)` |
| 4 | ret | n, value | r0 = value, `sp += n`, pop fp, pop return (sp == 0 ends the handler) |
| 5 | retv | n | as 4 without a value |
| 6 | movr0 | u8 reg | reg = r0 |
| 8 | arg | value | write value at `sp + pending`, `pending += size` |
| 9 | sp | n | `sp += n` |
| 0xa / 0xc | jz / jnz | rel32, value | jump if value false / true (int ≠ 0, float ≠ 0, vector ≠ 0, others true) |
| 0xd | jmp | rel32 | |
| 0xf / 0x10 / 0x11 | switch / case / endswitch | addr / rel32 value / – | case jumps when the value differs from the switch top |
| 0x12–0x18 | mul, div (B/A), mod, add, sub (B−A), shl, shr | binop | |
| 0x19–0x1e | B<A, A<B, B≤A, A≤B, ≠, == | binop | 1/0 |
| 0x1f–0x23 | and, xor, or, logical and, logical or | binop | |
| 0x24 | store | addr, value | with int↔float conversion |
| 0x25 | line | u32 | source line marker |

Events: the table after the code (h[5] entries of `{u8, u8 flags, u16 id, u32 codeOffset, f32}`) gives handlers for
event slots 0–63. Flags: 4 enabled (EnableState/DisableState), 2 pending. A signal sets pending on enabled
handlers (`FUN_100c6bc0`); pending handlers run each tick. Slot 0 is the start-up handler; timers (`SetTimer`) and
`Unit/Team/Mission/WorldSignal` raise slots; the engine raises fixed slots (1–5, 10, 11, 12, 27, 28, 31) for unit events.

Port status: `src/script/vm.*` implements the full instruction set and was run against every mission's scripts
(`hg2extract <data> scripts <MISSION> <seconds>`); `src/script/system.*` routes signals/timers and binds the
builtins below. The runner lists any builtin a mission calls that isn't bound yet (tactics/defcon, weather,
fountains, lights, teleport…).

## Mission UI builtins (0.6)

| Builtin | Arguments (offsets below the stack top) | Engine function |
|---|---|---|
| `SetUserObjective` ov0 | -8 slot, -4 status | `FUN_10133ad0` |
| ov1 | -0xc slot, -8 string, -4 status | `FUN_10133a40` (8 slots, text upper-cased) |
| ov2 | -0xc slot, -8 message index, -4 status | text from `FUN_1008d660` (mission UserMessageFile) |
| `SetUserCheckpoint` ov0 / ov3 | -0x1c id, -0x18..-0x10 position, -0xc, -8, -4 label (string / message index) | `FUN_100d77c0` / `FUN_100d7a50` → `FUN_10108320` (26 slots, default label `Check %d`) |
| ov1 / ov4 | -0x20 id, -0x1c unit name, position, -4 label | |
| ov2 / ov5 | -0x20 id, -0x1c unit reference, position, -4 label | |
| `Select/Next/Prev/KillCheckpoint` | -4 id (Select, Kill) | `FUN_10108520` … |
| `RegisterString` ov0 / ov1 | -4 string / message index | `FUN_101338c0`: centre text for 5 s |
| `SetFocusNotifyRange` | -4 range | the script unit's brain +0xbc |
| `SetUnitFocus` ov0 / ov1 | -4 unit name / reference; returns 1 if alive | `FUN_100b8200`; event **23** fires on the watching script when the focus unit comes within range |
| `GetUnitStatus` ov0 / ov1 | -4 unit reference / name; returns `UNIT_INFO` | `FUN_100d6850`: [0] external view, [1] posture (0 prone, 1 stand, 2 kneel, 3 SMS), [2] flag bit 20, [3]/[4] targeting, [5] state byte |
| `LockForwardStick` | -8 unit, -4 on | |
| `ControlCamera` ov0 | — (end) | `FUN_10092ff0(0)` |
| ov1…ov5 | -4 duration, -8 end signal, -0xc skip signal; path name at -0x10 (ov1), -0x1c (ov2, look point -0x18), -0x14 (ov3 unit name / ov4 unit ref at -0x10), -0x18 (ov5, look path -0x10) | camera paths are mission paths; a missing path raises the end signal at once |
| `SetCinematicControl` ov2 | -4 on | letterbox / cinematic mode |
| `SetHitpoints` | -8 unit, -4 hit points (engine stores `hp << 14`) | |
| `Invincible` | -8 unit, -4 on | unit flag bit 15 |

Message files (`Mission/<M>MSG.TXT`, named by the mission's 0x157 UserMessageFile) hold numbered lines `N :TEXT`.

## Engine events (`FUN_100b9fe0` and the damage code)

| Slot | Meaning | Raised by |
|---|---|---|
| 2 | dropped below a damage threshold | `FUN_10058fd0` |
| 3, 4 | part / system destroyed | `FUN_100618a0` |
| 5 | destroyed (one-shot flag) | brain +0xd0 bit 0 |
| 10 | — | `FUN_100b7ff0` |
| 11 | hit (one-shot) | brain +0x58 |
| 12 | being shot by the user (repeats while it lasts) | brain +0x5a |
| 13 / 14 | path finished / path event | brain +0xd0 bits 1/2 |
| 15 / 17 | enemy entered / left the enemy notify range (`SetEnemyNotifyRange`, ref 1 = that enemy) | +0x7c bit 0 |
| 16 / 18 | enemy detected / lost | +0x7c bit 2 |
| 19 / 21 | friend entered / left the friendly notify range (ref 2) | +0x68 bit 0 |
| 20 / 22 | friend detected / lost | +0x68 bit 2 |
| 23 / 25 | focus unit entered / left the focus range (ref 3) | +0xc4 bit 0 |
| 24 / 26 | focus detected / lost | +0xc4 bit 2 |
| 27 | brain flag +0x1b | |
| 28 | after a scripted custom signal (+0xcc) | |
| 29 / 30 | combat started / ended | +0x48 bit 0 |
| 31 | every brain tick | |

Script team numbers are mission teams minus 99 (99 neutral → 0, 100 the user's side → 1, 101 → 2…). Unit
references: 0 and 6 the user, 4 the script's unit, 1 enemy in range, 2 friend in range, 3 focus, 5 attacker,
7–0x11 / 0x12–0x17 the user's squad mates. Team alert (defcon) runs 0–5; `GetTeamDefcon` answers 6 for an
unknown team.

## Gear posture / movement (`.ECY`)

Each transition's event list starts with the `.XB` set to play when entering the target state. Triggers seen in every
gear `.ECY`: 1 stop, 2 walk, 3 run, 4 jump (take-off → airborne → landing), 6 posture down, 7 posture up,
16–20 falls, 24–27 switch between the four 37-state groups. From the idle state 0: walk 1, run 2, kneel 3, prone 4,
kneel-walk 5, crawl 6, jump 7 → airborne 9 → landing 8. State type 0 plays once and holds; types 1/2 loop.

## Operators (fixed in 0.10)
Binary operators (0x12–0x23) compute *first operand* op *second operand*: the memory operand comes first unless flag
0x20 puts the immediate first (`x - 1`, `count < 3`, `UserAlive() > 0`). Before 0.10 the port evaluated `-`, `/` and the
comparisons with the operands swapped.

## Open questions
- Unit references: ints 1–0x17 are relative references (self, user, leader, squad mates…, `FUN_100d3480`);
  strings are unit names (first 8 chars). Mapping to port unit ids is still approximate.
- `SetTimer` overload 1 takes an int count (treated as frames).
