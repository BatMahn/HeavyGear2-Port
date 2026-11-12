# Heavy Gear II data formats (reverse-engineered)

All findings traced from `hg2sim.dll` (1999-06-05 build, MSVC 5, no symbols).
Function addresses refer to that DLL (image base 0x10000000).

## Install layout (CD `Setup\data`)
- `simpack.prj` + `simpack.map` — sim asset archive (4615 entries: WAV, BMP, TGA, .M, XB, ECY).
- `hg2shell.prj` + `hg2shell.map` — shell asset archive (AVAR, ATXT, ALDL, AMSN, AWAV, ACHR, ANUM, APFI).
- `Setup\zip\zfs.zip` → `alp.zfs`, `rim555.zfs`, `rim565.zfs` (ZFS3 archives, shell art). Installer copies them into `Shell\`.
- `Terrain\*.MR~` — LZB-compressed `.MRM` terrains (engine tries `name.mr~` first, then `name.mrm`).
- `Mission\*.ANN` — compiled missions (token streams, see below). `Mission\*.txt` = briefing text.
- `Scripting\*.o` compiled mission scripts, `*.prj` script project lists (text).
- `where` — extension → search directory map. `HG2.cfg` — engine config (text).
- `MstrGear.bin`, `MstrWpns.bin` — master gear / weapon definition token streams.

## PRJ archive (`*.map` index + `*.prj` data)
map: `u32 count; u32 ntypes; ntypes × {char ext[4]; u32 n}; u32 0; u32 ndll; ndll × char[16] (zlib.dll, pkware.dll); count × {char name[8]; char ext[4]; u32 offset; char comp[4]}`
prj entry at offset: `"C:\HG2\" (7 bytes) 'S' char ext[4] u16 method (0=stored,1=zlib) u32 usize u32 csize data[csize]` (header 22 bytes).

## LZB compression (`FUN_1001d170`, used for `.MR~`)
Blocks: `u8 type; i16 blocksize (incl. 3-byte header)`. type 0 = stored, 1 = compressed, 0x80 = end.
Compressed opcodes (b = first byte):
- `b & 0x80`: copy `(b>>3)-13` bytes from `out - (((b<<8|n1) & 0x7ff)+1)` (2 bytes).
- `b < 0x40`: `b+1` literal bytes follow.
- `0x60..0x7f`: copy `n2+4` bytes from `out - (((b<<8|n1)&0x1fff)+1)` (3 bytes).
- `0x40..0x4f`: run of `b-0x3d` copies of n1.
- `0x50..0x57`: `((b<<8|n1)+0xb041)&0xffff` literal bytes follow.
- `0x58..0x5f`: run of `((b<<8|n1)+0xa813)&0xffff` copies of n2.

## MRM terrain (`FUN_100a5060`, `FUN_100a2380`, tile reader `FUN_100a0780`/`FUN_10096a20`)
- `u32 ntex (≤56)`, `ntex × char[128]` texture names (BMP).
- `16 bytes` header (`i32 x0, x1, z0, z1` = 0,4096,0,4096).
- 128×128 tiles (row-major, row = z), each:
  `i32 x0,z0,x1,z1` (32-unit tile rect), `i32 ×6`, 4 ignored, `i32 ×4 @+0x28..0x34`, ignored,
  `i32 step(+0x38)`, `f32(+0x3c)`, `i32 ×4`, `32 bytes (4 doubles)`; then `count(+0x2c)` u16 heights (×0.01) and `count` u16 attributes.
  Observed: count = 9 (3×3 grid, step 16), field +0x48 (index 19 of the 21 ints) = 0..3 (likely texture rotation).
- 128×128 × 4 × `i32` texture index (one per 16×16 quad of a tile; stored at tile +0xa0..+0xac).
- `u32 n (=256)`, `0xc00` bytes = 256 × vec3f table.

## Token streams (missions, definitions, MstrGear/MstrWpns)
Readers: `FUN_10016000` u32, `FUN_10016020` f64, `FUN_10016040` f32, `FUN_10016060` C string.
Blocks: `u32 id; u32 crc; u32 size; payload` where crc = CRC-32 (poly 0x04C11DB7, MSB-first, init ~0, final ~)
over the `size` bytes starting at the size field. Inside: `u32 x; u32 y;` then keyword tokens until `1` ('}').
Keyword ids come from the `{char*, id}` tables at 0x1015b098 etc. (see `re/out/kwtables.txt`).

### ANN mission (`FUN_1008dba0`)
`u32 crc; u32 size;` then 6×u32 (unused, off1, off2, unused, off3, extra_size), C string, u32 n1, n1 × {u32 size; block},
u32 n2, n2 × {u32 size; definition block}, then top-level token loop until 0:
- `0x18` Mission_Description: STR name, STR text file, 0x41 bytes, u32, u32.
- `0x1c` World_Status block: 0x1d Fog_Min F32, 0x1e Fog_Max F32, 0x1f Fog_Intensity F32, 0x20-0x23 fog RGBA U32, 0x24 Clipping U32, 0x25..0x29 light F32, 0x2a-0x2c light RGB U32.
- `0x31` Water_File block: 0x2d level F32, 0x2e alpha U32, 0x2f tidal F32, 0x30 anim F32, 0x44 texture STR.
- `0x32` Sky_File block: 0x44 texture STR, 0x45 geometry STR.
- `0x33` Terrain_File: STR (`D01.mrm`).
- `0x36` Scripting block (0x34 project STR, 0x35 library STR, 0xa1 file STR).
- `0x3a` User_Header / `0x3b` Object_Header → `FUN_10085570` (object instance, refers to definition n2[index]).
- `0xdf` Team_Config, `0xd2` Surfaces, `0x12a` FountainSystem, `0x157` UserMessageFile STR, `0xcb` (FUN_1008d160).

## .M model (`FUN_100110f0` → materials `FUN_10010800`, frames `FUN_100109c0`, BSP `FUN_1000dd00`)
`u32 crc; u32 size;` then `u32 matoff` (relative to its own position).
Materials at matoff: `u32 n; n × { f32[8]; u32 flags; STR name; STR texture }`.
Frames (at +4 after matoff field): `u32 crc; u32 size; u32 nframes;` each frame:
`STR name; u32 type; f64[12] (rows X,Y,Z,T; engine discards X and rebuilds an orthonormal basis from Y,Z: X=Y×Z, Y=Z×X); u32 child (1-based, 0=none); u32 sibling; i32 hasMesh (-1 none)`.
Mesh: `STR name; u32; f64[4]; f32[3]; u32 nuv × {f32 u,v}; u32 nv × vec3f; u32 nn × vec3f; u32 npoly; if npoly: u32 ncorner0; npoly × { u32 id; f32 plane[4]; ncorner × {u32 vertex, u32 normal, u32 uv}; u32 material (1-based); [u32 next ncorner] }; u32 hasBSP; if 1: bsp node = { u32 n; n × u32 poly; i32 left; i32 right; [left node if != -1]; [right node if != -1] }`.
World transform = local × parent (row vectors). Y is up. Verified on all 1029 models (exact length).

### Object instances (`FUN_10085570`)
`0x3a` (user/player) or `0x3b`: `i32 a; i32 b; [u32 if a >= 0 and 0x3a]; u32 crc; u32 size; '{'` then tokens to `1`:
- `0x3e` Object_Name STR · `0xa1` Script_File STR · `0x14f` WalkableGraph STR
- `0x3c` Object_definition_file: `'{' u32 objectClass (0x46 Regular .. 0x164) u32 defIndex (into table n2) u32 team (99 No_Team, 100.. Team_1..) ['}' if team]`
- `0x3d` Object_World_State: `'{' f64 x,y,z; f64 rot[3] (degrees, yaw first); u32 tok; if tok == 0x42: f64 quat[4]; '}'`
- `0x43` Light_Information: `'{' u32[3] f32[7] u32[3] '}'`
Definition blocks (table n2) are compiled definition files; geometry names inside may carry a leading `@` (shared geometry) which is stripped when loading.
Top-level tokens after objects: `0xd2` Surfaces, `0xcb`, `0x12a` FountainSystem (not yet parsed).

### Terrain tiles (update)
Tile grid is variable: `f[11]` = sample count, `f[12]`×`f[13]` = dims (3×3 step 16, or 5×5 step 8 for detailed tiles), `f[15]` = step.
For 3×3 tiles `f[19]` (0..3) is used as the texture rotation; detailed tiles hold `0x03020100` there (per-quadrant bytes — meaning TBD).
Only texture slot 0 of the 4 per-tile slots is used so far.

**Axes:** the file's first grid axis (tile index / 128, the rect's first coordinate, the outer loop of each tile's
heights) is the world **Z** axis, not X. Read that way, every mission's objects sit on the ground (mean |y − ground|
< 1 m on H01, H03, S01, D01) instead of 7–26 m off. `loadTerrain` transposes the grid on load; the texture UVs keep
the file's axes.

## .XB animation library (`FUN_10013a70`, sets `FUN_100143b0`)
`u32 crc; u32 size; u32 nsets;` each set: repeated `0x0d` Animation: `STR frame; u32 tok; while tok == 0x0e: { u32 keyType (0 rot, 1 scale, 2 pos); u32 n; n × { i32 time; f32[4 or 3] } ; tok = u32 }; u32 next` until next != 0x0d.
Rotation keys are quaternions stored (w,x,y,z); the engine's row-vector matrix equals the conjugate's D3DX matrix.
Gear rest frames in `.M` files are not a usable pose — gears must be posed from an animation set. Verified on all 13 `.XB` files (exact length).

## .ECY animation state machine (`FUN_10030960`, binary path)
`u32 crc; u32 size; u32 nsets; STR xbName; u32 (264); u32; u32; u32 nstates;` then per state
`i32 id; i32 type; u32 n;` and n transitions `{ i32 targetState; STR tag; i32 kind; i32 trigger; f64; f64; f32 rate; u32 k; k × {i32, f32, f32} }`.
Targets ≥ the state count with kind 3 and a part tag (e.g. `BDB`) are damage animations. Verified on all 13 `.ECY` files.
For the core states the state index equals the `.XB` set: 0 stand, 1 walk cycle, 2 run cycle, 3/4 crouch down/up, 5/6 prone down/up.

## Gear definitions (`FUN_1004bcf0`)
Top-level tokens until 0: `0x15a` Targeting_ID u32; `0x9d` Alternate_Geometry block (`0x9f` MAX_RADIUS f32, `0x9e` Alg STR × LODs);
`0x9c` Geometry_Files block (f32, STR); `0xa0` Animations_Script STR (.ECY); `0xa1` script STR; `0xa2` Hard_Point block
(`0xa4..0xac` HP1..HP8 + weapon keyword); `0x85` Damage_Hierarchy block; `0x88` Detection block (0x89-0x8e f32);
`0x8f` Stats block (0x90 Sensors, 0x91 FireControl, 0x92 Armor, 0x93 Size, 0x94 Maneuverability, 0x95/0x96 Movement u32;
0x97 MaxWalkSpeed f32, 0x98 MaxRunSpeed f32, 0x99/0x9a foot f32, 0x9b PerksFlaws u32 u32); `0xa3` Sound_File block
(0xaf motor, 0xb0 steps, 0xae destruction, 0xad damage, 0xb1 wheels STR); `0xad` u32; `0xb2` Detailled_Map STR.

`0x85` Damage_Hierarchy block (`FUN_1004bcf0`): entries `{u32 tok, u32}` until tok 1; tok `0x86` = `STR frame, u32 system,
u32 armour` (packed into the frame's flags as `(system & 7) | (armour & 0x3f) << 3`, bits 2..10), tok `0x87` = `STR`
damaged-skin texture (`damagec.tga`). Gears list 17 parts: BDA/BDB (structure, system 2, 63), BDC (sensor head,
system 3), RAA–RAD / LAA–LAD (arms), RLA–RLC / LLA–LLC (legs). Model frames carry a `frm-` prefix (`frm-bda`); the
rig also has `frm-POV` (pilot eye), `frm-HP1`…`frm-hp8` (hardpoints; HP1 hangs under the right arm), `frm-tst`
(torso twist) and `frm-ska`…`frm-skd` (skates).

## MstrWpns.bin (`FUN_1008da70`, entries `FUN_100ee440`)
`u32 crc; u32 size; u32 n; n × u32 offset` (relative to the size field). Entry: `u32 class;` then tokens until 0:
`0x6d` Header block (`0x7f` Weapon_Name STR, `0x72` POF STR, `0x73`-`0x76` sounds STR, `0x77` Next_Round_Time f32,
`0x78` Bullet_Velocity f32 (negative), `0x79`/`0x7a` Min/Max_Range f32, `0x7b` Burst u32, `0x7c` NextBurst f32,
`0x7d` Ammunition u32, `0x7e` Reticle u32, `0xad` Damages f32, `0x13c` Collateral_Radius f32, `0x152` Mortar_Angle f32,
`0x15c` Trail_Effect u32, `0x15d` Flash_Effect u32, `0x16d` Auto_Burst u32, flags `0x80` two-handed, `0x81` melee,
`0x82` grenade, `0x84` haywire, `0x159` remote grenade); `0x6f` Alternate_Geometry block (STR LODs);
`0x6e` Geometry_File (`crc, size, STR`); `0x71` Bullet_Geometry_File STR. All 200 entries parse exactly.

Gear `Hard_Point` entries are `HPn weaponKeyword count` (keywords 0xb3 LightAutoCanon … 0xc3 BattleAxe).

## Mission paths (`0xcb`, FUN_1008d160)
Tokens until `1`: `0xcc` Name STR, `0xcf` Rep_File STR, `0xcd` Anchor f32×3, `0xce` Control_Point f32×3 (relative to
the anchor), `0xd0` Line / `0xd1` Bezier segment `{ 0xcc name, 4 × 0xce, 1 }`. Paths follow the objects; `0xd2`
Surfaces and `0x12a` FountainSystem blocks before them are not decoded yet (the loader resynchronises on the next path).

## Script unit references (FUN_100d3480)
`0` = the user's gear, `4` = the unit running the script, `1/2/3/5` relations of that unit (leader/target…),
`7..0x11` user squad mates, `0x12..0x17` other groups. String arguments name units (first 8 characters).


## Particle System (`Particle System/{Hi,Med,Low}/*`)
Fixed-size records read whole (`fread`) by the loaders in hg2sim.dll; the detail folder follows the graphics setting.

- `.ptl` particle type (0x1ab4 bytes, `FUN_10126a30`): `char name[256]`; +0x100 f32 life (s); +0x104 f32; +0x108 u32
  flags (0x100 additive); +0x10c f32; +0x110 f32 gravity; +0x114 f32 (scaled at load); +0x118 `char texture[256]`
  (e.g. `effect2c.tga`); +0x2a4 u32 frames; +0x2a8 u32 blend mode (11 if frames > 1, 10 for `MUZZLE_FLASH.PTL`);
  five 256-entry f32 curves over the particle's life at +0x2ac (R), +0x6ac (G), +0xaac (B), +0xeac (A), +0x12ac (?),
  then the size curve at +0x16ac. The loader packs the colour curves into ARGB bytes.
- `.emt` emitter (0x928 bytes): `char name[16]`; 60 × f32 at +0x10; +0x100 u32 flags; +0x10c f32 emission time;
  +0x114 u32 particle count; +0x118 f32 speed; +0x11c f32 speed variance; +0x120 f32 cone angle (degrees);
  +0x124 f32; two 256-entry curves at +0x128 and +0x528.
- `.ped` effect (0x2698 bytes, `FUN_101272a0`): `char name[256]`; u32 n; n × 0x218 {`char emitter[256]`,
  `char particle[256]`, u32 flags, f32 scale, f32 delay, f32 velocity, f32, f32 cone}. Bytes after the n entries
  are uninitialised memory from the editor (`PSEDIT`).
- `.vf` vector fields (7 files) are not used yet.


## Mission blocks decoded in 0.9
- `0xd2` Surfaces (`crc, size, '{'`): `0xd3 count`, then `0xd6` entries `{0xd4 index, 0xd5 r g b, 0xd7 f32 friction,
  0xd8 u32 damage, 0x154 u32, 0x153 u32, 0x155 STR footstep wav, 0x156 r g b (dust), '}'}`. D01's surface 1 (yellow)
  does 50 damage (lava).
- `0x12a` FountainSystem (`'{'`, no size): system scalars `0x132` (3 = active), `0x12b`, `0x12c`; templates `0x12d
  {0x3c defIndex, 0x3f object type, [0xa1 script]}`; generators `0x12e {0x3e name, 0xa1 script, 0x6c team, 0x132
  active, 0x133, 0x12f mode 0..2, 0x130 total, 0x131 max alive 1..8, 0x12b wave 1..16, 0x134 flags, 0x40 x y z,
  0x41 rotation, 0x15e/0x15f/0x160 ranges, 0x135 template index (repeated)}` (binary parser at `FUN_1005bd??`, text
  form `FUN_1005b4a0`). `ActivateFountain(name, on)` toggles generators by name (`FUN_1005e2b0`).
- `0x43` Light_Information: `'{' r g b (u32), f32 range, f32 ×3, f32 inner / outer cone, f32 intensity, u32 ×3 '}'`
  on object type 0x5c; `ActivateLight(name, on)` sets +0x138 on the light.
- Paths may end without their `'}'` (the next `0xcb` starts) and segments without theirs.
- `0x39` RedBook_Track in World_Status is not used by any shipped mission.

## Damage model (`FUN_10058fd0`, `FUN_10059240`, `FUN_100594b0`)
Unit armour `A` = Armor stat ×10 (+0xbec, plus 2× a per-side modifier from +0xc34). A hit's damage `D` = weapon
Damages ×10 (`FUN_100ee440`) × the shooter's fire-control multiplier (table at 0x10163474, index FireControl+6:
0.24 0.2 0.3 0.5 0.7 0.8 1 1.5 2 2.5 3). `D ≥ 6A` destroys; `D ≥ 3A` is heavy (event 4, armour −20, critical roll
including 1-in-6 destruction); `D ≥ 2A` medium (event 3, −10, critical roll); `D ≥ 0.2A` light (event 2, −5); smaller
hits only raise event 2. Criticals: mobility −1/−2 (+0xbf4), manoeuvre −1/−2 (+0xbe8, turn rate), a random weapon
disabled (+0x2b4), destroyed.

## Shell data (0.10)
- **ZFS3** containers (`Shell\shellart\rim565.zfs`, `rim555.zfs`, `alp.zfs`; on the CD inside `Setup\zip\zfs.zip`):
  `"ZFS3"`, u32 version 1, u32 name length 16, u32 entries per directory block 100, u32 file count, u32 0,
  u32 first block. Block: u32 next block, then 100 × {char name[16], u32 offset, u32 index, u32 size, u32 time,
  u32 flags}. Stored uncompressed. The layouts name art as `X.TGA`/`X.BMP`; the shell reads `x.rim` (+ `x.alp`).
- **RIMF** image: `"RIMF"`, u32 1, u16 width, u16 height, u16 pitch (bytes), u16 1, then RGB565 rows
  (`rim555.zfs` holds the same art as RGB555).
- **ALPH** mask: `"ALPH"`, u32 1, u16 width, u16 height, u16 pitch, u16 0, then 8-bit alpha rows.
- **LDL** layouts (`hg2shell.prj`, root `HG2SHELL.LDL`): `import "x.ldl"`; `[template] name[:base:base…] { … }` with
  `int|bool|string key value` lines and child nodes; bases are other nodes (dotted paths such as `lows.back` reach
  children) whose values and children are inherited; `#` comments, `$` ends a file. `xpix/ypix/widthpix/heightpix`
  (or `…pct`), `xanchor/yanchor` center/right/bottom; buttons have `image0` (up), `activeimage0` (hover), `image1`
  (down), `disabledimage0`, `executesound`, `activatesound`; fonts are `fontname` (TrueType files in the data
  root) at `fontsize` points, colours `fontcolorred/green/blue` with `color0`/`activecolor0`/`color1`/`disabledcolor0`
  children per state. Screens are composed in code from top-level nodes (`splash_*`, `pilot_0..4`, `ln_brf_*`,
  `missions_*`, `dbrf_*`, `ia_*`, `low_*` buttons …); backgrounds are `hg2_asset_strings` (`SBksplsh`, `Sbkchrtr`,
  `Sbackgnd`, `SBAKTACT`, `SBAKBOTH`).
- Serialized shell objects (`.MSN`, `.CHR`, `GRINVTRY.TXT`, `WPINVTRY.TXT`, `GAMECHRS.TXT`, `.VAR`): u32 CRC, `'b'`,
  then fields; strings are u32 length + bytes.
- **MSN** briefing (`FUN_100d26be`): title, `.ANN`, mission id, next id (success), alternate id, forced chassis,
  forced variant; u32 squad size (gears incl. the user), u32, u32 fixed-squad flag; if 0: u32 n + n pilot names;
  u32 n + n "required" flags; u32, u32 space flag, u32 ×2 threat limits; intro movie + u32 w,h; outro movie + w,h;
  briefing movie (`Shell\movies`, Indeo 5 + PCM), still image, briefing, success, failure, alternate texts; then
  a fixed array of availability flags. The campaign runs S2J → S1J → S3J → S01 → S06 → … → S37 → `end`.
- `MissionOver(code, delay)` (`FUN_100d40f0`): script 1 = success (internal 2), 0 = failure (0), 2 = alternate
  outcome (1), which takes the MSN's alternate id and fourth text.
- **GRINVTRY.TXT**: `"grinvtry.num"`, u32 70, per chassis {name, `.stf`, u32, u32, u32 (1 = Black Talon squad gear),
  u32 weight class 1–4, u8, u8, picture `.TGA`, description, u32 n, n × {u8 hidden, u8, variant name, `.VAR`}}. Chassis
  *i* is `MstrGear.bin` entry *i* (that file: u32 CRC, u32 size, u32 0, u32 70, 70 × {u32 size, gear definition
  block as in missions}).
- **WPINVTRY.TXT**: u32 0, u32 200, 200 `.WPN` names in `MstrWpns.bin` order (then descriptions).
- **VAR** variant: after `'b'` and the u32 length, XOR-scrambled with key `23 22 70 97 7b 03 19` indexed by
  (remaining length) mod 7 (`FUN_1006a271`). Then chassis name, variant name, u32 weapon count, u32, and per weapon
  {u8 used, u32 hardpoint keyword 0xa4 + n, u32, u32 weapon class keyword, `.WPN` name, f32}; perks/flaws follow.
- **GAMECHRS.TXT**: u32 0, u32 11, {name, u8, `PIBIOxxx`}. **CHR**: full name, 7 × u32, voice `.WAV`, portrait
  (`SPPXxxxU/M/D.TGA`), favourite chassis, variant, biography.
- `S??LANCH.TXT` / `S??RETRN.TXT`: autosave descriptions (launch / return); stills `SSAVESnn` / `SSAVRSnn`.
- `INSTACTN.TXT` arena names; `INSTCTNF.TXT` their `.ANN` files (16-byte NUL-padded records).
- `CREDIT.TXT`: pages split by `<E>`; `<t:font>`, `<p:size>`, `<c:r,g,b>` and their closing tags style the lines.

## Team alert and dozing (0.10)
- Teams start at defcon 0 (`FUN_100b89e0(0)` in the brain constructor) and step up as alert points pass thresholds
  (each defcon level selects a 0x44-byte behaviour record), decaying back over time.
- `AutoSleep(unit, allowed)` (`FUN_10049a10`): with sleeping allowed (the default), every 2 s a unit outside the view
  frustum and over 300 m (`0x10141f30`) from the camera stops being simulated (`FUN_10049a40`).


## Decoded in 0.11
- **Terrain surfaces**: each height sample's u16 attribute word carries the surface type in bits 8..10
  (`(attr & 0x700) >> 8`, `FUN_100974e0`, nearest sample); `FUN_10078570` clamps it to the mission's `Surfaces`
  table and returns that entry. A gear whose feet touch a surface with a damage value takes it through its contact
  handler (vtable +0x8c, from `FUN_1003db20`); D01's lava is surface 1 (`0x1c8` attribute words).
- **Emitter curves** (`.emt` +0x128 / +0x528): 256 steps over the emission time — emission rate in particles per
  second, and a speed factor for new particles. The count at +0x114 is the most particles one emitter keeps alive.
- **ECY hit reactions**: triggers 12 / 13 / 14 (light / medium / heavy) are kind-3 transitions with a part tag
  (`BDB`, the body) to targets past the state count; their event names an `.XB` set (20..22 standing, others per
  posture / weapon) that plays over that branch of the model while the legs keep their own animation.
- **Movies**: `Setup\movies\*.avi` (title, intro and the campaign cut-scenes named in the `.MSN`: intro movie
  before the briefing, outro movie after a successful debrief) and `Shell\movies\sc*.avi` (briefing SATCOM video)
  are Intel Indeo 5 (`IV50`, YVU9) at 320×180, 15 fps, with 22 kHz PCM sound.
- **Gear lab tables** (`hg2shell.prj`, loaded by `FUN_100ed???` at shell start): `MAXMPTBL`, `MNVRTABL`,
  `ARMORTBL`, `WPNTABLE`, `AMMLDTBL`, `PERKSTBL` (perk / flaw costs as small RPN-like expressions over `R`, `Crew`,
  `Passenger`, … tokens at 0x101d4d30), `TGTMLTBL`, `MNVMLTBL`, `SNSRRTBL`, `FCRATTBL`, `HPDSCTBL`. They are the
  Heavy Gear construction-system tables behind threat values.

## Threat value (0.12, `FUN_100bf7bf`)
- `.VAR` record (`FUN_100c5420`): chassis, variant, u32, u32, 7 × weapon slot `{u8 used, u32 hardpoint, u32
  WPNTABLE id, u32 class, STR .WPN, f32 rounds}`, then 7 × i32 stats (+0xf4 sensors, fire control, armour, size,
  manoeuvre, movement 1, movement 2), u32 perk bits (+0x110), u32 perk/flaw bits (+0x114), 11 × i32 perk ids.
- `WPNTABLE`: 90 × `{u32 id, STR name, i32 base cost, f32 cost per round, i32, u32}`. `PERKSTBL`: `{u32 id, STR name,
  STR expression}`; expressions are postfix (`FUN_100e1017`, tokens at 0x101d4d30) over R (rating), H_Range, Size,
  Crew, Passenger, Limited, WpnCount with `+ * -`, `RndDown`/`RndUp` (truncate) and `a b c if`. `TGTMLTBL`
  (fire control −5..3) and `MNVMLTBL` (manoeuvre −10..10): `i32 lo, i32 hi`, then f32 multipliers.
- Offensive = (Σ weapon base + 2·(size + 2·arms − 2)²/4 + Σ rounds × per-round cost) × TGTML[fire control], where arms
  are perk bits 27..28. Defensive = (armour² + 0.013824·max(MP)³ + min(MP)²) × MNVML[manoeuvre]. Miscellaneous =
  103 + max(0, sensors + 1 + perks − flaws)², the perk sum following `FUN_100c00aa`'s bit fields (see
  `GameData::perksAndFlaws`). Total = ⌈(max(1,O) + max(1,D) + max(1,M)) / 3⌉; each part is shown rounded up.
- The `.MSN` threat pair is the per-gear limit and the squad total (`dlg_exceed_gear_max_threat` /
  `dlg_exceed_max_threat`). Stock Black Talon standards come out at 2343–2404 against S2J's 3000.

## Tactic table (`FUN_100dadc0`)
Named tactic objects by unit class (2 gear, 3 infantry, 4 frame, 10 hover): GearCombat1, GearCombat3 (circle),
GearCombatHand2Hand, GearFlankLeft / GearFlankRight (`FUN_100a9770`, ±75°), GearPursue1, GearPatrol1, the space
variants (GearSpaceFlank1..3 with formation offsets (0.8, 0.2, −0.2), (1.0, 0, −0.4), (0.8, −0.2, −0.2)), and the
hover / infantry ones. Each feeds goals to a lower-level manoeuvre planner (`FUN_100d9620`).

### Tactic think functions (vtable slot 11, decoded 0.19)
Each tactic is a small state machine over a run record (+8 step, +0xc timer, +0x18 counter) that posts orders with
`FUN_100d9620` (order kinds from `FUN_100bdb90`: 1 STAND, 2 MOVETO, 6 TURNTO, 10/11 FIREWPN, 0xc POSITION …) and
waits for the gear's order queue (+0x21c) to drain. The port runs the same machines (`src/game/ai.cpp`):

| Tactic | Function | Behaviour |
|---|---|---|
| GearCombat1 | `FUN_100a84d0` | turn to target; no shot → MOVETO target; ≥32 m: 20 % FIREWPN 2 s else run laterally ±64 m; <32 m: 10 % volley else 64 m sidestep at ±90° |
| GearCombat2 | `FUN_100a8dd0` | hold home, TURNTO + FIREWPN 3 s when visible, else back to home |
| GearCombat3 / HoverCircle | `FUN_100a90e0` | re-plan every 2 s to a random point 32 m round the target |
| Flank* | `FUN_100a9ee0` | approach at ±75° (Left1/Right1 ±45°, Left2/Right2 ±90°, Frontal 0°), volley 5 s |
| Hand2Hand | `FUN_100aacc0` | close to 8 m |
| StandAndShoot | `FUN_100a8ba0` | POSITION by range, TURNTO, FIREWPN 4 s |
| TurretCombat1 | `FUN_100aaef0` | FIREWPN 5 s |
| TankAttack | `FUN_100ab040` | 80 % volley 5 s / 20 % reposition |
| DefaultRunAway | `FUN_100ab550` | turn away, move 30 m |
| HuntUser | `FUN_100ab880` | MOVETO the user, re-plan once the user is >128 m from the goal |
| HuntTarget | `FUN_100aba50` | MOVETO target ±16 m random, re-plan past 64 m |
| GearPatrol1 / VehiclePatrol / FreeRange | `FUN_100abee0` / `FUN_100af590` / `FUN_100af2f0` | random point round home (64 m), MOVETO, STAND, wait ~3 s |
| Pursue | `FUN_100af9b0` | MOVETO target, STAND, wait 3 s |
| HoldGround | `FUN_100afb50` | TURNTO the threat, STAND, wait |
| PathFollow1 | `FUN_100ae440` | from the nearest node of the assigned path, MOVE_DIRECT to a neighbour 50/50 back or on, clamped at the ends; the table's default non-combat tactic for a unit with an `AssignPath` path |
| DroneAttack1 | `FUN_100b06e0` | alternate rush / break off on random timers |
| PathFollow2 | `FUN_100ae910` | a general tactic switched on the unit's mode (+0x44: 0 non-combat, 1 combat, 2 post). Non-combat: walk the nodes in turn with a random offset. Combat (`FUN_100aed10`): MOVE_DIRECT along the path, firing on the move, a node at a time toward the node nearest the target that is still at least the free-range radius from it (neighbours weighted ×0.8, `FUN_100ae820`); at that node, drift within the radius |
| SquadMateFollow | `FUN_100ad160` | formation slot; parameters 32 / 4 / 16 m: set off past 16 m (4 m with the leader moving), settle inside 4 m, run past 32 m |
| SquadMateDefend | `FUN_100adc20` | radius 32 m: MOVETO the charge at speed 5 when ≥0.6 r away until inside 0.4 r; beside a still charge wander within r, STAND, wait 3 s; take on whoever hit the charge in the last 6 s |
| SquadMateGuard | `FUN_100ae0e0` | MOVETO the guard point while getting closer, STAND once no nearer (used for Hold / Go To) |
| FreeRange1 | `FUN_100af2f0` | wander round home within the `ActivateFreeRange` radius (`FUN_100d80e0`: +0x1ac radius, +0x1b0 on) |

`AssignPath` (`FUN_100d5a90` → unit vtable +0x22c) only stores a path; the PathFollow tactics read it back (+0x230).
MOVETO (`FUN_100373c0`, gear vtable +0x2b0) asks a pathfinder for waypoints (`FUN_100bf020`; four search variants
`FUN_100c3190` / `FUN_100c2ae0` / `FUN_100c5730` / `FUN_100c5250` picked by request flags, results 0 none, 1 path,
3 empty, 6 no planner, 7 gave up). The follower (`FUN_100bf300`) pops waypoints inside the arrival radius (+0x150) and
keeps a progress watchdog: the nearest distance so far (+0x24) and a 5 s timer (+0x20) reset whenever it gets
nearer; when the timer runs out the move fails. The port has the same watchdog (one planned re-route first, then the
order is dropped so the tactic picks again).

The search (`FUN_100c3190`, decoded 0.22): A* over a 4 m grid (`DAT_102689d0`) with 8 neighbours. A step costs its
squared length (16, diagonals 32) plus the squared height change; under water it also costs (depth + 1) × the step
(`FUN_100c26a0`). The heuristic is the squared distance to the goal, so f = g + d². Nodes are expanded best-f first,
128 a frame (+0x7c), 2048 in all (+0x78); a node inside the goal radius ends the search, otherwise the best node
found becomes the end of a partial path (result 3). Walkability (`FUN_100c2520`): the occupancy grid's cell flags
(bits 2–5: 1 open, 2 blocked), water deeper than the request's limit (+0xb8, when wading is off), ground whose normal
is steeper than the request's slope limit (+0xc4, cosine). Straight-line tests (`FUN_100c3530`) walk the segment in
2 m steps with a 4 m radius. The goal radius comes out of `FUN_100c0370` (1 + 0.5 × cell = 3 m).

The request's limits: MOVETO allocates the follower with `FUN_100bec30(slope)`, which builds the request with `FUN_100bff80(slope)` → request +0xa4 (copied to
the search's +0xc4); `FUN_100c0220` then fills +0xa0 with the world pointer (`DAT_1026667c`), +0x98 with the depth
limit and +0x14..+0x1d from the flags. The walkability test is `|n·(0,1,0)| < +0xc4` → blocked, with n the terrain
triangle's normal (`FUN_100a4200`, up vector `DAT_102689d8`), and water deeper than +0xb8 (when the depth limit is on).
Flag values: 2 → +0x1d, 4 the other goal-radius rule (`FUN_100c4670`), 8 clears the depth limit, 0x10 the water
penalty, 0x20 puts nodes on the water surface, 0x40 skips the slope test. Per class (type codes of the object factory `FUN_1005cdb0`):

| Class (type) | MOVETO | slope limit | depth limit (+0x98) | flags |
|---|---|---|---|---|
| Biped gear (0x48), Infantry (0x50), ChickenWalker (0x52), Frame (0x53) | `FUN_100373c0` (vtable +0x2b0) | constant 0x3f5db22d = cos 30° | the unit's bounding-sphere radius (object +0x98, `FUN_10017fa0`) | 0, 1 with the order's 0x80 bit, \|8 when Amphibious (perk word +0xc30 bit 1) |
| Jeep (0x4e) | `FUN_1006cad0` / `FUN_1006cf50` | unit +0x27c = cos 20° (`FUN_1006c700` → `FUN_10049db0(20)`) | bounding radius | 0 / 1 |
| HoverTank (0x4f) | `FUN_10068c30` / `FUN_10069180` | cos 30° | bounding radius (unused) | 0x38, 0x39 with the 0x80 bit |
| Mount (0x51) | `FUN_100730a0` / `FUN_10072cd0` | cos 30° | bounding radius | 0 / 1 |

`FUN_10049db0(deg)` stores cos(deg) at +0x27c; the base vehicle default is cos 0.349 rad = 20° (`FUN_10048ad0`), gears,
ChickenWalkers and Mounts set 30° (`FUN_10035460`, `FUN_10041220`, `FUN_1006f620`), but only the Jeep's MOVETO reads it.
So gears and the other walkers avoid ground steeper than 30° and water deeper than their radius (unless Amphibious) and
pay (depth + 1) × step for wading; only hovers path across water freely.

Slope force on a grounded gear (`FUN_1003cc30`, gear vtable +0x268, summed in the physics step `FUN_1003af10`): with
the ground normal n (+0xfc0, from `FUN_10095940`) turned into the gear's frame, the force on each horizontal axis is
n·|n| × k × mass × 20 (g, `DAT_10163ae8`), k = min(3 / n_y², 6) or, on skates (+0x13e0, getter vtable +0x2d4),
min(5 / n_y², 9). Where n_y > 0.8 (flatter than 36.9°) the sideways part is dropped, and the forward part too while
the throttle (+0xf48) is zero. The drive and resistance forces are capped at the traction μ × mass × g with μ = 2
(+0xff0, `FUN_1003c8f0`), the slope force is added after that cap: a walking gear can't climb faces much steeper than
~39° (3 tan²θ × 20 = 40), a skating one ~32°, and a gear standing on ground steeper than 36.9° slides. Standing on an
object needs a contact normal · up > 0.7 (`FUN_1003db20`). The port applies the same numbers to a slide velocity
(only the part beyond the traction on an axis the gear is driving along), damped by its own drag.

Life Support (perk 22, bit 26 of the perk word +0xc30): nothing in `hg2sim.dll` tests it; it only counts toward the
gear's threat value in the shell. Backup Life Support (bit 4) is the crew-critical backup. The port plans its own: an A* over 4 m cells round ground steeper than the unit's slope limit, deep water, solid
objects and lava, string-pulled to a few waypoints, when a unit is stuck or the next 48 m of its line are blocked.


## Network protocol (the port's own, 0.13)
UDP, little endian, every packet starts `u32 'HG2N', u8 version (1), u8 type, u8 sender id`. The host (id 0) relays
every client packet to the other clients with the sender id filled in. Types: 1 HELLO `{name, gear}` (client, once a
second while in the lobby), 2 WELCOME `{u8 id}`, 3 LOBBY `{arena, i32 frag limit, i32 time limit, u32 seed, u8 n,
n × {u8 id, name, gear, i32 kills, i32 deaths}}` (host, twice a second), 4 START (the lobby payload), 5 STATE
`{u32 seq, vec3 pos, f32 yaw, twist, speed, turn, strafe, u8 run, posture, skates, airborne, alive, f32 armour}`
(20 Hz), 6 FIRE `{vec3 muzzle, vec3 aim}`, 7 DEATH `{i32 killer}`, 8 LEAVE, 9 END, 10 PING. Strings are `u8 length` +
bytes; a gear is `{i32 chassis, i32 armour, i32 fire control, u8 n, n × {u8 hardpoint, u16 weapon}}`.

## Perks and flaws in the simulation (0.15)
The gear object keeps the definition's Stats: +0xbe4 sensors, +0xbe8 fire control, +0xbec armour × 10, +0xbf4
manoeuvre, +0xbf8/+0xbfc movement, +0xc30/+0xc34 the PerksFlaws words (same bit layout as the shell's variant record).
Decoded uses: turn rate (0.6 + 0.05 × manoeuvre) × base (`FUN_1003aaf0`); sensors compared in detection
(`FUN_101108b0`); haywire effects skipped with Haywire Resistant (bit 17, `FUN_100594b0`); ECCM × 100 against a
missile-lock roll (`FUN_101117f0`); the critical-hit tables (`FUN_100618a0`) where Backup Fire Control (bit 3),
Reinforced Chassis (flaw word bit 8), Reinforced Crew Compartment (bit 9), Emergency Medical (bit 16) and Rugged
Movement System (bit 30) absorb a critical once, and the exposed-system flaws shift the roll; random system hits
(`FUN_10062180`) take a level off ECM (bits 10–12), ECCM (13–15), jump jets (24–25) or stealth (flaw word 10–12).
The port's sizes for the other effects (detection, scatter, damage percentages) are its own.

## Tactic commands
Tactic objects post command 0xc to the gear (`FUN_10036920`), which sets the pilot controller's stance word (+0x1d0):
low nibble 0..2 and high nibble 3..6 with a float (the standard combat tactic passes the selected weapon's range,
`FUN_100b7d00`). `FUN_100649d0` / `FUN_10064c50` turn the stance into ECY triggers (0x1d..0x25).

## Static collision (port, 0.24.x)
The .M models carry no collision hulls - only per-mesh render BSPs (`FUN_1000dd00`) - so the port derives volumes from
the drawn triangles (`src/assets/collision.cpp`): opaque surface samples (alpha-tested texels removed) are binned into
vertical columns, each column keeps its occupied height ranges (gaps a gear fits under stay open), small wall-enclosed
hollow shells without floors are filled, and matching columns are merged into boxes, grouped in clusters for queries.
Models wider than 200 m (Gomorrah's city, `epl10005.m`) get one grid per mesh. Object classes from the factories
(`FUN_10085570` / `FUN_100883d0`): Object_NoColl (0x162) clears the object's collision flags (`[4] & ~0x10000`,
`[5] & ~0x40000`) and Plane_NoColl (0x61) `[5] & ~0x40000`; Ghosts_Object (0x13d) only clears its targetable flag
(`+0x1a`) and still collides. Tree_Object (0x47) instances collide with the part rising from the base (the trunk).
`hg2extract <data> collaudit` compares the volumes with the old one-box-per-mesh scheme; `HG2_COLLVIEW=1` draws them.
