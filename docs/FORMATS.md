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

**Cell diagonals** (`FUN_10097670` normal / `FUN_10097ed0` height+plane, terrain tile vtable 0x10147100 +0x34 / +0x2c,
queried by `FUN_10095940`): each grid cell is two triangles, and bit 15 (`0x8000`) of the attribute word at the cell's
low corner picks the diagonal: set → (ix, iz)-(ix+1, iz+1), clear → (ix+1, iz)-(ix, iz+1). The triangle is picked by
the nearer of the two off-diagonal corners; height and normal are that triangle's plane (no smoothing, no averaging).
Most cells have the bit clear (H01: 3162 of 65536 set), so the default split is the anti-diagonal. The port's
`Terrain::sample` / `normal` and the drawn mesh follow it.

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

### Particle system behaviour (decoded for 0.27.x)
- **Types** (`FUN_10126a30`): +0x100 life, +0x104 extra life (a particle lives `life + rand·var`, `FUN_10125fb0`);
  +0x108 flags: bit 0 drag, bit 1 gravity, 0x100 additive (material `0x15 | 0x400`); +0x110 drag rate; +0x114 gravity
  factor, ×9.8 at load and added to the vertical speed (smoke has positive values and rises); the size curve is
  halved at load (it is the sprite's half-size at scale 1). +0x21c 16 × {u, v} cell origins per frame, +0x29c / +0x2a0
  cell width / height (negative heights flip); +0x1aac bit 31 = the air velocity is the wind; +0x1ab0 frame cycles over
  the life (0 in the 0x1ab0-byte files, which keep frame 0). Mode 0 with frames > 1 becomes 11; `MUZZLE_FLASH.PTL` 10.
- **Motion** (`FUN_10127920`, table at `0x10296ca8` filled by `FUN_10127a50`): index `(flags & 3) | 4 if wind`:
  0 plain, 1 drag to rest, 2 gravity, 3 drag + gravity, 4/6/8 still, 5 drag toward the wind, 7 wind + gravity; a
  plain particle slower than 0.01 is still. Wind air = `wind × (1 − on × (b − 128) × gust / 128)`,
  `b = byte(int(dot(pos, wind)) + phase)`; the phase steps `int(dt × 64)` a frame and the gust switch `on` (starts 1)
  flips each time `(count & 15)` seconds have passed (count +1 per flip) (`FUN_10124ee0`). At the high setting
  (`FUN_10127e90` / `FUN_101286b0`) a wind particle within 5 m of the ground bounces its air velocity off the slope
  ahead (`air += n × (n·air) × −3` when heading into it). Steps are 1/30 s, at most 5 a frame.
- **Effects** (`FUN_101260c0`): .ped entries {emitter, particle, u32 attach (follows the object passed), f32 scale,
  scale variance, velocity, velocity variance, cone}; each makes an emitter of scale `(s ± var) × caller scale`; a
  velocity over 0.01 sets the emitter moving along a random unit vector bent into the cone (cos ≤ 0: negative y
  scaled by −cos; else x, z × sin and y = |y|). Lone particles at +0x2284 name types no folder ships. Names without
  a .ped make nothing (`FUN_10124480`).
- **Emitters** (`FUN_10123480`, `FUN_10123600`, `FUN_10123bd0`): .emt +0x104 shape (0 point, 1 cone shell, 2 ring),
  +0x108 drag, +0x10c / +0x110 emission time ± variance, +0x124 radius; flags 2 inherit the emitter's velocity, 0x10
  loop, 0x20 gravity, 0x80 drag, 0x100 the emitter flies. Particles per frame = the rate curve integrated over the
  emission-time fraction since the last emission (`FUN_10124030`), spread along the emitter's path, capped by the
  count alive (+0x178, decremented as they die, `FUN_10124180`). Speed `(speed ± var) × speed curve × scale`; shape 1/2
  offset by `radius × speed curve × scale`.
- **Drawing** (`FUN_10128ac0`): camera-aligned squares (`FUN_10128fd0`, no spin, no near fade) of half-size
  `size × scale`; frame `int(age × anim × frames) % frames`; modes 0/11 plain, 1/9 tinted RGB, 2/9 sprites filling the
  gap to the emitter's previous particle (`FUN_10129230`, `1 + dist / (2 × size)` of them, cells rotating), 3 screen
  sized (`size² × scale × depth / focal`), 4 flat, 5 flat on the water (+0.1), 6 sprite under water else flat on it,
  8 sprite above water else flat, 10 attached; others are not drawn.

## Weather (`FUN_100f6c90` .. `FUN_100f8630`, object vtable 0x1014b4d0)
- `SetWeatherType(t)` (`FUN_100f6e80`): sim +0x50 = No/Rain/Snow_Weather (0x14c..0x14e); a rain or snow object is
  made and the level reset to 0. `SetWeather(level)` (`FUN_100f6ec0`): the particle setting caps the level
  (`FUN_100f7110`: low drops the tier, medium caps it at 2); level % 5 picks {rain speed, snow speed, grid side,
  drops drawn, rain sound}: 1 {60, 4, 5, 500, Envrain2}, 2 {80, 7, 7, 750, Envrain2}, 3 {90, 9, 11, 1000, Envrain1},
  4 {100, 12, 15, 1500, Envrain1}, 0 off; levels 5..9 add lightning at 100 a second, 10..14 at 600 (`FUN_100f7560`).
- Drops: an N × N field falling along `normalize(1, −3, 0)` at the speed; each lives 1..3 s (snow 3..5) and starts
  again at `(32r/N × j, y, −32q/N × j')` (cell r = i % N, q = i / N; j, j' = ±16/N jitter **multiplied** in;
  y = lifeMin × speed .. 50) (`FUN_100f7a20`); snow wobbles `cos(left / life × 10π) × 0.2` per frame.
- The field is drawn for every visible terrain tile (`FUN_10096fe0` → `FUN_100f7f00`) at (tile x, tile max height,
  tile z), up to the frame's allowance. Rain (`FUN_100f8390`): an untextured sliver from the drop 5 m along the fall,
  1 pixel each side at the far end (640 × 480 terms), white at alpha 20. Over a tile with samples below the water a
  drop under the surface (+0.08) draws a ring there instead (`FUN_100f8630`, effect2c.tga 0.5..0.75 × 0..0.25, colour
  0xd2dce6ff, half-size `((1 − left/life) × 0.9)²`). Snow builds its triangle from three points on one line, so the
  flakes draw nothing.
- Lightning (`FUN_100f7b50` / `FUN_100f7c40`, rain only): a counter grows by `max(1, int(rate × dt))`; once over
  0x1fff a strike happens when `rand() + 1 < counter`, then 1000..60000 is taken off (≤ 0x7fff, else 0). The strike is
  50..180 m from the camera with envthnd1.wav there; the bolt object (`FUN_100f0400`, vtable 0x1014a6a0) is not drawn
  by the port.
- Water (`FUN_100a5790`): level = base + sin(Water_Tidal_Rate × t) × 0.09.

## Sound (EAR, `earias.dll` / `earpds.dll` from `Setup\iasinst.exe`)
- Sounds.lst (`FUN_1012d380`, `%i` fields): defaults minimum 0, maximum 10000, outer 10000, inner 0, channel 1;
  `EAR_PERF_LOOP` moves a channel below 5 to 2. Positions go to EAR listener-relative × 100 (`FUN_1012bf2e`), so ranges
  are centimetres.
- Level at a distance (`earpds FUN_10004504`, EAR_PERF_DISTANCE): maximum inside the inner range, minimum past the
  outer, linear between. DirectSound volume (`FUN_1001528c`): x = level / 100 (× gain) and
  `mB = −100 × (60 − 2.73303x + 0.0586456x² − 0.00058625x³ + 2.131e−6x⁴)`.
- Channels (`FUN_1012e708` → RegisterChannel): 2 (loops) 16 voices at the high sound setting (8 medium, 4 low), 3, 4, 6
  likewise, 7 and 9 half. Every object plays its own event (`FUN_1012f608`). Which events EAR drops when a channel is
  full, and its matrix panning, are not decoded.
- Footsteps (`FUN_10067570` → `FUN_10036310`): the surface's footstep wav else the gear's; unless the foot is under
  water, FOOT_DUST0/1 by the surface DustIndex in its DustColor (none for other indices), FOOT_RAIN / FOOT_SNOW in rain
  / snow; at the unit's position, scale = its radius, only radius > 2, within 250 m of the camera, not at the low
  particle setting.


## Mission blocks decoded in 0.9
- `0xd2` Surfaces (`crc, size, '{'`): `0xd3 count`, then `0xd6` entries `{0xd4 index, 0xd5 r g b, 0xd7 f32 friction,
  0xd8 u32 damage, 0x154 f32 MovementCost, 0x153 u32 Flags, 0x155 STR footstep wav, 0x156 r g b DustColor,
  0x165 u32 DustIndex, '}'}` (binary reader `FUN_10079960`, 0x48-byte entries). D01's surface 1 (yellow) does 50 damage
  (lava). `FUN_10078570` returns entry + 0x28, so a bullet's ground hit reads DustColor (+0x18, packed 0x00RRGGBB) and
  DustIndex (+0x1c byte) from it. (Before 0.26.1 the port stopped at 0x165 and lost the surfaces after it.)
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

## Team alert and dozing
- The team object (`FUN_100b8330`) starts at alert level 1 (limits +0x12 = 1, +0x14 = 5; `SetMinDefcon` / `SetMaxDefcon`
  move them). Each level selects a 0x44-byte behaviour record (`FUN_100b89e0`: team config +0x248 + level × 0x44,
  filled by `FUN_100bb970`; every brain's +0x14 points at it). Changing level clears the alert points (+0x18) and sets
  the time left at the level (+0x1c) to the record's +0x38.
- Alert events (`FUN_100b8900`): points 1 / 10 / 25 / 5 / 25 / 100 / 50 for events 0..6 — 2 a unit entering combat
  (`FUN_100b9ae0`), 3 an enemy round passing close (`FUN_100553f0`), 4 a unit hit (`FUN_100b82a0`), 5 a unit destroyed
  (`FUN_10062980`). The time left grows by half the points (up to +0x38); points over the record's +0x3c raise the level.
  At the end of every brain pass (`FUN_100ba7c0`) the points drain at 0.5 / s and the time left by dt; when it runs out
  the level drops one.
- Records (level: sensor reach +0x00 (`FUN_100b8fc0`, clamped 50..400), engage range +0x0c, seconds in view before
  engaging +0x10, gunner rest +0x18, hit memory +0x1c, post-combat time +0x28, throttle non-combat / combat / post
  +0x2c..+0x34, time at level +0x38, points to rise +0x3c; +0x40 the engage test `FUN_100bddd0`):

  | lvl | +00 | +0c | +10 | +18 | +1c | +28 | throttle | +38 | +3c |
  |---|---|---|---|---|---|---|---|---|---|
  | 0 | 400 | 48 | 0 | 0.1 | 8 | 10 | 1.5 / 1.5 / 1.5 | 1e30 | 1e30 |
  | 1 | 100 | 64 | 8 | 3 | 5 | 10 | 0.3 / 0.85 / 0.3 | 1e30 | 100 |
  | 2 | 150 | 80 | 5 | 2.7 | 5 | 10 | 0.4 / 0.85 / 0.5 | 20 | 150 |
  | 3 | 250 | 120 | 2 | 2 | 10 | 10 | 0.5 / 0.85 / 0.6 | 40 | 100 |
  | 4 | 300 | 250 | 1 | 1 | 10 | 10 | 0.6 / 0.9 / 0.8 | 30 | 100 |
  | 5 | 400 | 300 | 0.5 | 0.5 | 20 | 20 | 0.8 / 1.0 / 0.9 | 15 | 1e30 |

  (+0x04, +0x08, +0x14, +0x20, +0x24 are filled too — 1/1/1.5/2/3/1e30, 3/2/2/1/0.5/0, 8/1/8/15/20/30,
  64/64/120/180/256/1e30, 0.25/0.05/0.25/0.5/2/0.95 — their readers are not traced.)
- `AutoSleep(unit, allowed)` (`FUN_10049a10`): with sleeping allowed (the default), every 2 s a unit outside the view
  frustum and over 300 m (`0x10141f30`) from the camera stops being simulated (`FUN_10049a40`). No other exceptions.

## Unit brain (gear +0xd8c, `FUN_100b77a0` defaults; per frame `FUN_100b9ae0`)
- Defaults: visibility range +0x54 = 250 m (`SetVisibilityRange`), enemy notify range +0x74 = 180 m
  (`SetEnemyNotifyRange`), friend notify +0x60 = 64 m (`SetFriendlyNotifyRange`), focus +0xbc = 180 m
  (`SetFocusNotifyRange`). `CanTarget(unit, on)` sets brain +0x1a: whether *other* units may target this one.
- Perception (`FUN_100ba1c0`): over the sensor's contact list (+0x1cc +0x1e8), same-team units within the visibility
  range give the nearest friend (+0x5c / +0x64); other-team, targetable units within it whose sensor signal (+0xc50,
  set by the sensor code `FUN_10111xxx`) reaches their concealment (+0xc2c, 0.15 standing; halved by object flag bit 20)
  give the nearest enemy (+0x70 / +0x78). The notify events fire when those distances cross the notify ranges. +0x3c
  counts the time an enemy has been in view, capped at the record's +0x10, and drains at 2 / s.
- Target (`FUN_100b9300`): a target locked by `Attack` (+0xa4, `FUN_10050440`) is kept; otherwise the target is held
  for the team's +0x23c (5 s) and then chosen again: the attacker (+0x1e0) if it hit within the record's +0x1c, else
  by the team's targeting mode (+0x22, default 2: `FUN_100bcaf0`, the contact with the highest
  `-distance + 5 × Σ loaded weapons' damage + armour points (+0xbec) − 0.5 × last frame's score (+0xd4c)`
  (`FUN_100502d0`), × 100 when it is showing on the sensors; mode 1 `FUN_100bc9e0` the lowest, mode 0 the nearest),
  else the nearest contact; with none it looks again in 0.5 s. +0x20 keeps where the target was last seen.
- Engage (`FUN_100bddd0`): a target showing on the sensors (+0xa5), not holding fire (+0xe6), and: hit within +0x1c, or
  a friend's attacker within +0x1c (+0x1d8), or a near shot this frame (+0xca), or the nearest enemy within the
  record's +0x0c, or (unless +0xe7) an enemy in view for the record's +0x10.
- Modes (+0x44, 0 non-combat, 1 combat, 2 post-combat): 0 → 1 when it engages (alert event 2), 0 → 2 with a target it
  doesn't engage; in combat a 6 s timer (+0x4c; 2 s for flag +0x194 bit 4 on the user's side) restarts while it
  engages and its end starts the post mode with the record's +0x28 (+0x50); in post mode engaging cuts the time to 2 s,
  and when it runs out the unit goes back to combat (engaging) or non-combat. Cinematic mode without
  `AllowCinematicCombat` holds every brain in mode 0. COMBAT_START (29) / COMBAT_END (30) are raised on entering and
  leaving mode 1 (`FUN_100b9fe0`, brain +0x48 bit 1).
- Tactics: on a mode change, or when the tactic's Think returns false, `FUN_100b9510` takes the script's tactic for the
  mode (combat +0x1a0 `SetCombatTactic`, post +0x1a4, non-combat +0x1a8 `SetNonCombatTactic`; `SetGeneralTactic` sets
  all three; `Attack` makes HuntTarget the non-combat one) if it applies, else `FUN_100dd910` picks from the class's
  list for the mode the applying tactic with the highest weight − uses (weights are 0, so the least used; the first
  registered wins ties) and counts the use; else the default list (combat DefaultRunAway, post DefaultHoldGround,
  non-combat DefaultPatrol). Class lists (`FUN_100da380`): gear (2) combat GearCombat1, GearCombat3,
  GearCombatHand2Hand, GearFlankRight, GearFlankLeft / post GearPursue1 / non-combat GearPatrol1; infantry (3)
  InfantryCombat1, InfantryCover / InfantryPursue1 / InfantryPatrol1; frame (4), mount (5) MountCombat1 /
  MountPursue1 / MountPatrol1; chicken walker (6) ChickenWalkerCombat1, ChickenWalkerCombat2 / Pursue1 / Patrol1;
  plane (7); turret (8) TurretCombat1; vehicle (9) TankAttack / JeepPursue / VehiclePatrol1; hover (10)
  HoverCombat1, HoverFlankRight, HoverFlankLeft, HoverCircle / HoverPursue1 / Pa_t1_c3..Pa_t3_c3. Registered for any
  class with mode flags (combat, post, non-combat): PathFollow1/2 (1,1,1), FreeRange1, SquadMateFollow / Defend / Guard,
  ChaseEnemy, HuntUser, HuntTarget (0,1,1), Frontal, FlankRight1/2, FlankLeft1/2, GearCombat2 (1,0,0).
  Applies (+0x20): most combat tactics need a usable weapon (`FUN_100bc870`: best of `Auto_Burst × damage² − +0x1d0`);
  GearCombatHand2Hand needs a melee weapon and no target 16 m or more away while a gun is usable (`FUN_100aa8d0`);
  DefaultRunAway: hit in the last 6 s, out of non-combat mode, or an enemy in view (`FUN_100ab460`).
- GearCombat1 / InfantryCombat1 / HoverCombat1 / FrameCombat1 (`FUN_100a84d0`, start `FUN_100a8440`: four rounds, a
  POSITION order with the mode's throttle): each round, with the target in sight: not facing it (75°, +0xa0 bit 4) →
  TURNTO; in reach (+0xa9): ≥ 32 m 20 % FIREWPN_V 2 s, else a run across its front (its position + its heading × 32 m
  ± 64 m sideways, MOVETO flags 0x199, 4 s); < 32 m and it isn't facing the unit: 10 % FIREWPN_V, else a 64 m move at
  90° round it (0xb9, 4 s, backwards); < 32 m and it is facing the unit: the run across; out of reach → MOVETO it
  (flags 1) until in reach, holding combat (+0x4c = 2). Target out of sight: MOVETO where it was last seen (flags 9),
  STAND, until it shows.
- GearCombat3 / HoverCircle (`FUN_100a90e0`): every 2 s it checks there's a weapon; a move (0x109) to where the target
  was last seen (not in sight / reach), a random point within 32 m (32 m or more away), or 32 m straight away from it
  (closer), then back to where it started.
- Order parameters the tactics post (`FUN_100d9620(run, kind, flags, param)`): Flank* MOVETO 0x109 / 0x189, TURNTO 9,
  FIREWPN_V flags 10, 5 s; TurretCombat1 / TankAttack FIREWPN_V 5 s (TankAttack moves 9); GearCombat2 TURNTO, FIREWPN_V
  3 s, MOVETO home 1; StandAndShoot FIREWPN_V 4 s; Hand2Hand MOVETO 1 then NOP 3 s; DefaultRunAway MOVETO 0x211 30 s;
  Pursue / Patrol MOVETO 1, STAND, NOP 3 s; HuntUser / HuntTarget MOVETO 1; SquadMateDefend / Guard MOVETO 1;
  PathFollow1 MOVE_DIRECT 8, PathFollow2 in combat 0x108.

## Orders and the gunner
- Executors (`FUN_10039f40` by kind): 0 NOP waits `param` s; 1 STAND (`FUN_10036ea0`) stops; 2 MOVETO (`FUN_100373c0`);
  6 TURNTO (`FUN_10038490`); 10 FIREWPN_V (`FUN_100371d0`): stops (+0x158(0,0)), makes the order's object the target
  (`FUN_100503c0`) and holds the trigger (+0x1ac) until the order's `param` runs out or the target dies; 11 FIREWPN_A
  (`FUN_10036bc0`): holds it for `count × Next_Round_Time + Burst × NextBurst` (weapon +0xec, `FUN_100efd50`).
- MOVETO flags (+0x28): 1 the goal is the point, 2 the goal is the order's object, 8 hold the trigger every frame while
  the brain is in combat mode (`FUN_100b7c60`: +0x44 == 1), 0x10 the move ends after `param` s, 0x20 drive backwards,
  0x100 flip 0x20 once the target is more than 135° (`DAT_10140bd8`) off the nose (latched until it comes back),
  0x80 the pathfinder's wading flag. The throttle is the brain mode's from the record (`FUN_100b7d00`); the order's
  `param` is only a time. The gear drives on when the heading is within two turn steps of the goal, else turns on the
  spot with the throttle at 0. FOLLOW_PATH uses brain +0x1d4 (`SetSpeed`: `kph × 0.278 / top speed`, clamped ±1, or a
  tactic's POSITION order).
- Gunner (`FUN_1003ab40` → vtable +0x12c `FUN_1002ef10` while AutoAim +0xe8, whatever the mode, when there is a
  target): `FUN_10036f10` aims the torso twist and every weapon at the target's lead point (`FUN_100b6960`): its
  position plus its velocity × flight time × 1.25 (flight time = distance / Bullet_Velocity; none when the velocity is
  under 1e-5). No random scatter.
- Whether a held trigger lets a round go (`FUN_10050500`, vtable +0x180, into the weapon's +0xf8): never while holding
  fire (+0xe6) or without a target; after the burst limit (+0xdc = Auto_Burst, at most the rounds left) the gunner rests
  (+0xe5) for the record's +0x18 + rand − 0.5 s (`FUN_100b7cd0`); otherwise only with a line of sight (+0xa0 bit 2,
  `FUN_10053d10`: terrain samples from the first weapon to the target) and the weapon on (+0x118 `FUN_100efaf0`: melee
  always; else the lead point within Max_Range and within the target's bounding radius (+0x98) of the gun's line,
  `FUN_100b63f0`), and no same-team or neutral unit nearer than the target + 5 m in the line of fire
  (`FUN_100b7e00` → `FUN_100bab20` / `FUN_100b64a0`).
- Weapon cadence (`FUN_100ece00` / `FUN_100ef270` / `FUN_100ecd40`): Next_Round_Time (+0x1a0) between rounds; after
  Burst (+0x1b0) rounds a NextBurst (+0x1bc) pause, then Next_Round_Time again. Auto_Burst (+0x1b8) is only the AI's
  burst length and part of the weapon score.
- Squad commands (`FUN_100d1bc0` for the four mates, +0x128 + i × 0x140): fire at will / return fire / hold fire set
  brain +0xe7 / +0xe6 (`FUN_100cfd00`: 0 → both clear, 1 → +0xe7, 2 → both); Go To places each mate at its
  formation offset round the nav point (`FUN_100cef60` → `FUN_100be4f0`, formation tables `FUN_100be020`, scaled by
  `SetUserSquadSeparation`); Attack / Defend My Target pass the user's target (`+0x24` / `+0x28`).

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
| GearCombat1 | `FUN_100a84d0` | four rounds of: turn to target; out of reach → MOVETO it; ≥32 m: 20 % FIREWPN_V 2 s else a run across its front; <32 m: 10 % FIREWPN_V else a 64 m move at 90° (see "Unit brain") |
| GearCombat2 | `FUN_100a8dd0` | hold home, TURNTO + FIREWPN 3 s when visible, else back to home |
| GearCombat3 / HoverCircle | `FUN_100a90e0` | a move firing on the way (last seen spot / random point within 32 m / 32 m away), then back to the start point (see "Unit brain") |
| Flank* | `FUN_100a9ee0` | approach at ±75° (Left1/Right1 ±45°, Left2/Right2 ±90°, Frontal 0°), volley 5 s |
| Hand2Hand | `FUN_100aacc0` | MOVETO the target, wait 3 s |
| StandAndShoot | `FUN_100a8ba0` | POSITION by range, TURNTO, FIREWPN 4 s |
| TurretCombat1 | `FUN_100aaef0` | FIREWPN 5 s |
| TankAttack | `FUN_100ab040` | in sight: 80 % FIREWPN_V 5 s / 20 % a move firing on the way; else at it |
| DefaultRunAway | `FUN_100ab550` | drop the target, turn away, MOVETO 30 m away (at most 30 s); the combat default when nothing else applies (no usable weapon) |
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

Gear ground physics (`FUN_1003af10`, the gear class's vtable 0x101434a0 +0x24c; per-gear constants from its init
`FUN_10060270`, m = the def's Maneuverability `0x94`, all forces × mass so they read as accelerations). The step is at
most 1/10 s. On the gear's local velocity (z forward = -z model axis, x sideways):
- drive (`FUN_1003c0b0`, +0x258): drivek × throttle on z, drivek = 20 + 2m (+0xf58), throttle (+0xf48) -1..1.
- forward resistance (`FUN_1003c5f0`, +0x260): r = v / vmax, vmax (+0x270) = top speed byte × 6 × 0.278 (skate speed
  `+0xbfc` on skates); r × 4 with +0x1249 and × 10 with +0x124a clear (stand / posture changes: `FUN_10064d20` sets
  +0x124a in the moving states). Past r = 2 the whole velocity is scaled by 1 / r. Force = dragk (r|r| + 2r with no
  throttle), dragk = 20 + 2m (+0xf60), limited to |v| / dt so it never reverses the speed. Steady state on the flat:
  r = √throttle.
- sideways resistance (`FUN_1003c750`): sidek × clamp(v_x, ±20) / vmax, sidek = 400 + 30m (+0xf68), same |v| / dt limit.
- traction cap (`FUN_1003c8f0`, +0x270): the horizontal sum of the three above is scaled down to μ g = 2 × 20 (+0xff0,
  `DAT_10163ae8`); airborne it is zeroed.
- gravity (`FUN_1003d1f0`), then the slope force (`FUN_1003cc30`, +0x268) **after** the cap: with the ground normal n
  (+0xfc0, the terrain triangle under the body's centre from `FUN_10095940`, or the contact normal of an object stood
  on, `FUN_1003db20`, which needs n · up > 0.7) in the gear's frame, n_h |n_h| × k × g on each horizontal axis,
  k = min(3 / n_y², 6), on skates (+0x13e0, getter +0x2d4) min(5 / n_y², 9). Where n_y > 0.8 (flatter than 36.9°)
  the sideways part is dropped, and the forward part too while the throttle is zero.
- with no throttle and no turn, |v| < 0.05 (`DAT_10163af0`) puts the body at rest.
So a gear standing on ground steeper than 36.9° slides (the slope beats the 40 of traction) until the ground levels
out, and with the drive at most 20 + 2m a gear can't climb faces past ~30° (60 tan²θ = 20) - the same 30° slope
limit the path planner uses for gears. The port asks for a speed rather than a throttle, so it sets the throttle a
pilot would: the flat-ground value (target / vmax)², plus what holds the speed against the slope, clamped to full.

Locomotion animation (`FUN_10064d20`, gear vtable +0x2fc, called first in the physics step): in the walk / run states
it advances the animation (anim object vtable +0x78), takes the root's travel over the step (the difference of the
anim's root position before and after, +0x30 / +0x48), cancels it on the displayed root, and sets the cycle's rate to
speed × FUN_100344e0 = speed / stride (fast inverse square root of the cycle's root travel, 8 when the travel is under
0.125) through `FUN_10055290` - one loop per stride travelled. Its result (root travel − v_z dt, plus the sideways
travel) is added to the position when grounded, so the body moves exactly by the animation's root motion and the
planted foot stays put however the speed changes. Walk / run is picked from the speed (`FUN_10064700`): ECY trigger 1
(stop) under 1 % of vmax with no turn, 2 (walk) up to the walk speed (+0x10b4), 3 (run) above it.

.ECY transition events `(set, a, b)`: walk → run and run → walk are kind 1 with events at a = b = 0.05, 0.30, 0.55,
0.80 (sync points: the switch waits for the cycle to reach one of the phases a and starts the target cycle at b);
stand → walk is kind 1 `(1, 0, 0.3)`; walk → stand is kind 0 with a = 0.15, 0.25, 0.35, 0.65, 0.75, 0.85. The
transition's f32 is 35 (walk → run, walk → stand), 15 (run → walk, stand → walk). The runtime that consumes them is a
table-built machine (`FUN_10030960` → handlers `FUN_10030310`..`FUN_10030920` emitting opcode edges through
`FUN_10016230`), not decoded further; the port takes the sync points as above and cross-fades 0.2 s on stand <-> walk
and walk <-> run (the stand pose's feet are ~1 m from any walk pose).

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

## Cinematic mode and scripted cameras
- `+0x521` on the sim (`DAT_10164724`) is cinematic mode. Writers: the constructor (`FUN_1008331c`, 0),
  `SetCinematicControl(on)` (`FUN_100d67b0` → `FUN_10092f70`: also sets the letterbox scale `DAT_101d3ab0` to 1/15 or
  1.0), and the view switch `FUN_10092ff0` cases 0 (cockpit) and 1 (external; not case 8, the death camera), which clear
  it. `FUN_10092ff0(0/1)` runs from `ControlCamera()` (`FUN_100d5ef0`), the user's internal/external view key
  (`FUN_10093490`, `DAT_10297090`, handled even while `+0x520` user control is off), the player switch `FUN_10093e60`,
  the tactical map closing and a gear flag check in `FUN_10094700`. A camera path running out does **not** end it.
- Readers: reticle / HUD (`FUN_10134ba0`), brains (`FUN_100b9ae0`: targets only when `+0x521 == 0 || DAT_10268824`,
  the `AllowCinematicCombat` flag), map key (`FUN_10093490`), sim speed scale `FUN_10076780`, sounds.
- Camera objects: `+0x283c` (ov2, case 2), `+0x2840` (ov1, case 3), `+0x2844` (ov3/ov4, case 4), `+0x2848` (ov5, case 5).
  Setup (vtable +0x18 → `FUN_1007d2d0`) stores the script's `-0xc` argument at `+0x14` and `-8` at `+0x18`; `-4` > 0
  is a timer at `+0xc` (flag `+0x10`). `finished` (vtable +0x14, `FUN_1007d2a0`) = timer < 0, else the path-end flag
  `DAT_101fce48`. Each frame `FUN_10093490` raises `+0x14` (end signal) once when the current camera finishes, then
  clears both (`FUN_1007c4e0`); the camera stays the view. `FUN_10092ff0` switching from one camera object to another
  raises its `+0x14` if finished, else `+0x18` (interrupted). A missing path / unit raises `-8` at once.
  So `ControlCamera(path, ..., unit, endSig = -0xc, skipSig = -8, time = -4)`; S1J's end handler (signal 37) does
  `ControlCamera(); SetUserControl(1); SetCinematicControl(0)`, its skip handler (38) only restores control.

## SetWeaponConstraints (`FUN_100d6b90` / `FUN_100d6c10` → vtable +0x1a4)
- `(unit, a, b, c, d)` degrees × π/180 (`DAT_101486b4`); only turrets implement +0x1a4 (`FUN_100ec400`): pitch joint
  `+0xd98` limits `[-a, b]`, yaw joint `+0xd9c` limits `[-c, d]` (joint `+0x16c` min, `+0x170` max, angle `+0x164`).
- Turret setup `FUN_100eb0c0`: pitch joint "JXA" on frame `-XA` about the object's X basis, default ±30°
  (`0x3f060a90`); yaw joint "JYA" on frame `-YA` about Y, unlimited (±1e30). Joints (`FUN_1006e360`, tracker
  `FUN_1006e910`) turn by quaternion keys at 0, 90, 180, 270° about the axis (`FUN_10022640`); the quaternion matrix
  (`FUN_100226c0`) is the standard one stored row-major and applied to row vectors, i.e. a rotation by −θ. With models
  facing −Z: positive pitch angle = gun down, positive yaw angle = toward the unit's +X, its left. So elevation runs
  −b .. a and the turn to the right −d .. c. Shipped scripts (S01, S09, S27) use symmetric yaw (±90°); S01 sets pitch
  (20, 30) and (40, 50).

## Artillery, markers, unit contact, script effects (0.25.x)
- **ArtilleryStrike(pos, delay, count)** (`FUN_100d8b20` → the "Artillery" object `FUN_100348b0`): +0x138 delay,
  +0x13c elapsed, +0x140 shells fired, +0x144 count, +0x148/+0x14c a 0.5 s interval/timer. Once elapsed > delay the
  timer runs down and each time it passes 0 a shell is made (`FUN_10034a50`; the delay grows by 0.2 per shell, which
  never catches up). The shell (vtable 0x10140a08, base `FUN_100e51e0`) carries **no MstrWpns weapon**: it starts at
  rest 150 m above the script's point, offset by `rand() % 30000 × 0.001 − 15` m in x and z, falls under 20 m/s²
  (`FUN_100e7910`), has ammo class byte 0x1f (+0x1d8; class 3 mortar, size 1 medium) and blast size 30 (+0x1cc),
  flags/damage word 0x190080 (+0x154). The class byte indexes the effect tables `FUN_100e4ca0` fills at start
  (`0x1026935c` 8-byte rows {trail row, explosion row}; trails `0x10269470` 3 × {S,M,L}: MTMI, MTBA, MTMO, MTPA;
  explosions `0x10269330` pairs {ground, object}: MEMIG/MEMIO, MEMOG/MEMOO, MEGRG/MEGRO, MEING/MEINO), so the shell
  trails MTMOM and bursts as MEMOG on the ground (vtable +0xe8) or MEMOO on an object (+0xe4), with `expdyna2.wav`.
  The burst (`FUN_100f3140`) is an object of radius 30 living 30/60 = 0.5 s; everything it touches (`FUN_100f36e0`)
  takes `(word >> 14) × 2` = **200** damage (Damages 20, no falloff, event 0xe) after distance / 60 s
  (`FUN_100f3430`), plus a 6000-unit push over 0.3 s that the port leaves out.
- **Spawn points** 0x56–0x59 (team 1–4, unit class with vtable 0x10146580, +0xd98 = team) only join the world's spawn
  list (+0x2860) and are never added to the drawn object list; **Light_Information** (0x43) makes the light
  (`FUN_1001aed0`, spot when the first flag is 1), copies the object's name and placement to it and releases the object
  (its `lampicon.m` is the editor icon). Neither is drawn in game.
- **Unit contact**: all unit classes share the vehicle base flags (`[4] | 2`, `[5] | 0x10010000`, `[4] & ~0x10000`).
  The world collision pass (`FUN_100a0b00` / `FUN_100214c0`) calls vtable +0x88 on both objects: gears note the other
  object at brain +0x1b4 (`FUN_10036280`), the vehicle base sets +0x130 (`FUN_100045c0`), Jeeps (0x101449b8) and
  hovertanks (0x10144368) have the empty `FUN_10004590`. Nothing pushes units apart; the port gives Jeeps and hovers
  hit boxes that do not block movement.
- **SetSmoking(unit, level)** (`FUN_100d3a40`): unit +0xbe0; non-zero → `FUN_100561e0(level − 1)` attaches DAMAGEL (0)
  or DAMAGEH (1; anything else → 0) at 1.5× scale to the damage frame or 0.65 × radius above the origin; 0 removes it.
- **SetGlobalWindVariables(vec, gust)** (`FUN_10124c00`): wind at 0x1026d200, |wind| at 0x10296bb0, gust 0x10296bc8.
  Start-up value (6, 0, 0), gust 1.5 (`FUN_101246e0`). Particles relax toward `wind × (1 − gust × (b − 128) / 128)`
  with `b = byte(dot(pos, wind) + phase)` at their type's +0x110 rate (`FUN_10127c90`).
- **AutoScan(unit, on)** (`FUN_100d7470` → brain +0xe9; set by default, cleared while a target is assigned
  `FUN_100503c0`, set again on dropping it or AutoPilot): the idle sweep `FUN_10037e30` turns the torso at π/2 rad/s
  (+0x13b8) to yaw goals of `rand × π/2 / 32767` on alternate sides and pitch goals of `rand × 0.3 / 32767`, resting
  `0.5 + 3 × rand / 32767` s at each.

## Weapon effects (`FUN_100e4ca0`, 0.26.1)
- **Effect tables**, built once by `FUN_100e4ca0` (called from `FUN_100e4fd0`, which also loads `expdyna2.wav` and
  `envfirea.wav`). An *ammo class* byte `c` (projectile +0x1d8, set through vtable +0xf4 = `FUN_100e5300`) is read as
  row `c / 10`, size `c % 10` (0 S, 1 M, 2 L); row ≥ 8 or size ≥ 3 falls back to row 0.
  - Rows `0x10269358` (8 × 8 bytes: u32 trail colour 0x00RRGGBB, byte trail row, byte explosion row):

    | row | colour | trail row | explosion | weapons (Trail_Effect codes in MstrWpns) |
    |---|---|---|---|---|
    | 0 | 0x000000 | 0 (none) | MI | launchers with no code (LRP128, -1 → 0 in `FUN_100e8570`), thrown grenades |
    | 1 | 0xffffff | 1 MTMI | MI | rocket pods LRP/VLRP 10, MRP 11, HRP 12 |
    | 2 | 0x969696 | 3 MTBA | MI | bazookas LBZK/RFB 20, MBZK 21, HBZK 22 |
    | 3 | 0x000000 | 4 MTMO | MO | mortars LFM/LGM/APM 30, MFM 31, HFM/HGM 32; artillery shell 0x1f |
    | 4 | 0x323232 | 4 MTMO | GR | grenade launchers APGL 40, LGL 41, HGL 42 |
    | 5 | 0xffffff | 5 MTPA | MI | panzerfausts LPZ 50, MPZ 51, HPZ 52 |
    | 6 | 0x323232 | 1 MTMI | IN | incendiary rocket pods IRP 60, HIRP 62 |
    | 7 | 0x000000 | 2 MTMI(M,L,L) | MI | guided missiles ABM/AGM 70, ATM 71, AAM/HATM 72 |
  - Trails `0x10269470` (6 rows × {S, M, L}): none; MTMIS/MTMIM/MTMIL; MTMIM/MTMIL/MTMIL; MTBAS/MTBAM/MTBAL;
    MTMOS/MTMOM/MTMOL; MTPAS/MTPAM/MTPAL.
  - Explosions `0x10269330` (5 rows × {ground, object}, `FUN_100e4c20` pairs): MEMIG/MEMIO, MEMIG/MEMIO,
    MEMOG/MEMOO, MEGRG/MEGRO, MEING/MEINO.
- **Trail** (`FUN_100ef740`, weapon vtable +0xdc, only when the weapon's Trail_Effect (+0x2c0, keyword 0x15c) ≠ -1):
  the effect is attached to the projectile (`FUN_101260c0` with it as parent) with the row colour; in space every
  trail is MT_SPACE, a projectile starting under the water level gets MT_WATER. The colour replaces the RGB of particle
  types with draw mode (+0x2a8) 1 or 9 (`FUN_10128ac0`), keeping the curve's alpha: MTMI / MTBA (mode 9) are tinted,
  MTMO / MTPA (modes 0 / 2) are not. So guided missiles trail black smoke, IRPs dark grey, bazookas grey.
- **Explosive projectiles** (vtable 0x101498f0 family: 0x101499e8, 0x10149ae0, 0x10149bd8, the shell 0x10140a08):
  +0xe4 object hit (`FUN_100e53e0`) → the row's object explosion, +0xe8 ground hit (`FUN_100e5530`) → ground
  explosion, +0xec end of flight (`FUN_100e5880`) → object explosion where it is. Each makes the explosion object
  `FUN_100f3140` (scale 1.0, radius = Collateral_Radius +0x1cc) and plays its copy of `expdyna2.wav` there.
- **Bullets** (cannons, machine guns, lasers, rail guns: `FUN_100dece0`, vtable 0x101487a8; laser beams 0x10149048
  share its handlers): object hit `FUN_100df390` → SPARKO; ground hit `FUN_100df4e0`: above the water level the
  surface's DustIndex picks SPARK0 (0) / SPARK1 (1) tinted with its DustColor, any other index SPARKO; under the
  water SPARKU with the dust colour. (Names resolved by `FUN_101258e0` into `0x10269048..54` in `FUN_100deb40`.)
- **Muzzle flash** (`FUN_100ef610`, weapon vtable +0xd8, only when Flash_Effect +0x2c4 ≠ -1; 0x15d values > 9 become
  -1): table `0x10163488` = MUZZLE_FLASH for 0-3 and 6-9, FLARE_SMA for 4 (lasers), FLARE_LAR for 5 (HLC / HPLC);
  flash 1 adds four more MUZZLE_FLASH at fixed offsets. FLARE_LAR has no file in any shipped particle folder (the
  original finds nothing either; the port shows FLARE_SMA larger). Rocket and missile launchers have no flash.
- Draw mode 3 particles (SPARKO, MT_SPACE) are sized on screen: size × depth / projection factor (`FUN_10128ac0`).
- Weapon class by hardpoint keyword (`FUN_10049f40` factory): 0xb3/0xb5/0xb6/0xc0 base `FUN_100ecbf0` (bullets),
  0xb4/0xba/0xbb/0xbc/0xbd launchers (`FUN_100e8570` / `FUN_100e8a30`, projectile from vtable +0x130 carrying the
  ammo class), 0xb7/0xb8/0xb9/0xbe lasers (`FUN_100e2b10` family, a beam object), 0xbf rail gun (`FUN_100eada0`).

## FollowPath (`FUN_100d5340` .. `FUN_100d5730`)
- ov0 (x, y, z): the script's own unit to a point; ov1 (unit ref -0x14, point) / ov2 (unit name -0x14, point); ov3
  (path -8): the own unit along a path; ov4 (unit ref -0xc, path -8) / ov5 (unit name -0xc, path -8). Points go to
  `FUN_100d5380` (brain order 2, flags | 0x241), paths to `FUN_100d5590` (order 5, flags | 0x240) on the unit's brain
  (+0xd8c). The user's gear has a brain too: S1J's s1j_user script sends it along "user1" while cinematic mode runs
  under the camera1 fly-by; the port walks the player's gear along the path while the user has no control (cinematic
  mode or SetUserControl(0)) and drops the order when control comes back. Dropships (class 0x55, "Plane",
  `FUN_1002a730`) fly their path's points (S1J "dship"); the port moves them at the def's run speed (else 40 m/s).
- **Maglev trains** (class 0x13e, `FUN_10073a00`, vtable 0x101452b8; M10 / M20): AssignPath (+0x22c,
  `FUN_10073bd0`) stores the path's points as offsets from the train's own position (relative to the path); Forward(n)
  / Backward(n) (+0x224 `FUN_10073b10` / +0x228 `FUN_10073b70`) set the direction (+0xdb8 = ±1), the point count
  (+0xdbc) and the next point; the update (`FUN_10073d10`) moves speed × dt toward it, stepping on until the count runs
  out or the path ends. The port does the same, with SetSpeed in km/h (default 20 m/s). Elevators (class 0x60,
  `FUN_10073ef0`, vtable 0x10145508; S21 / S22) derive from it with the same +0x224..+0x22c slots; their update
  (`FUN_100740a0`) clamps the time step to 0.06 s before `FUN_10073d10`.
- Class 0x5d craft (`FUN_1002df40`; S30's drones / maintenance craft) take FollowPath like dropships in the port.

## Mission sweep notes (0.26.1: every mission 30 s windowed, `--verbose-scripts`)
- Geometry names longer than 8.3 (`pAbpg20005.m` S25 / S27, `pAmrd30005.m` H07 / H09 / S27, `pAmbo10005.m` S37,
  `pAbwr10005.m` S27) have no file of that name; the shipped files are the last 8 characters of the base (BPG20005.M,
  MRD30005.M, MBO10005.M, BWR10005.M). The port falls back to that (a heuristic: the original's lookup for these was not
  traced). Decoded since: the model loader (`FUN_100110f0`) strips the extension and asks the archive
  (`FUN_10130b20` → `FUN_10130f10` / `FUN_10131370`) with the upper-cased base, and the archive's binary search
  compares only the first 8 characters (`FUN_101314f0`): "PABPG200" against "BPG20005" matches nothing, and the direct
  file fallback fails too, so how the original showed these objects is still open; the port keeps the fallback.
- T05_PATROL1..4 call FollowPath("patrol1".."patrol4"), but T05 only has the paths cam1, user1 and failcam: the original
  data has no such paths either (`FUN_1008c960` finds nothing and the order is skipped).
- `redbkNNN.wav` cues (TriggerSound in many scripts; Sounds.lst marks them "#CD audio track N") have no files: CD audio.
- FLARE_LAR (Flash_Effect 5) is missing from every particle folder.
- S27_GATE1 assigns "gatepth1", which S27 doesn't have (data). Still unsimulated: S22's bldg1frm on a class 0x53 frame
  object and the fountain-spawned VTOL (vtol1path), which AssignPath but aren't units the port moves.

## Gear locomotion, pilot input and criticals (0.27.x physics audit)
Gear class vtable 0x101434a0. All of this replaces earlier port estimates in `player.cpp` / `combat.cpp`.

**Speeds** (`FUN_10035460`, `FUN_10064700`, `FUN_10063a10`): vmax (+0x270) = Movement (+0xbf8) × 6 × 0.278 m/s
(DAT_10143408, km/h → m/s); on skates (ECY states 9 / 0x2e / 0x53 / 0x78) the second Movement (+0xbfc) × 6 × 0.278.
Walk / run boundary +0x10b4 = MaxWalkSpeed, or 0.35 vmax when 0; +0x10b8 = MaxRunSpeed or 0.65 vmax. Criticals lower
the Movement words, so vmax follows them.

**Pilot input** (`FUN_10063a10`, action names from the key table at `FUN_10135...` init, 0x10297xxx):
Forward + Backward axis → throttle +0x131c; steering with no axis drives at 0.05; below −0.5 it is held at −0.5.
Throttle setting +0x1b18: 1/3 (0.333), 2/3 (0.667), Full (1), Increase / Decrease ±0.05 a poll (to 1 / down to 0.05),
Full Stop (0, forward mode). Reverse Direction toggles +0x1b1c (1 = forward, the initial value). Forward mode: an axis
in 0 .. setting becomes the setting; reverse mode: axis − setting × 0.5. The result and the steer axis go to vtable
+0x158 (`FUN_1003aaf0`): turn command = steer × (0.6 + 0.05 × Maneuverability +0xbf4), throttle stored at +0xf48.
Jump → ECY trigger 8 (only on the ground, no thrust left); Jump_Jets → 9 (also needs perk bits 24-25); Crouch → 6;
Stand_Up → 7; Wheels → request +0x1314 (needs Movement2 > 0), sent as trigger 4 once |v| ≤ 0.25 vmax, the throttle
held at 0 meanwhile.

**ECY states** (group 0 of four 37-state groups, triggers 24..27 switch groups): 0 stand, 1 walk, 2 run, 3 crouch,
4 prone, 5 crouch-walk, 6 crawl, 7 skates on → 9 skating (type 2) → 8 skates off → walk, 12 → 13 → 14 Jump (to run),
15 → 16 → 17 Jump Jets (to run), 18 (trigger 0x10, airborne physics), 19 / 20 heavy-hit falls (0x11 / 0x12),
21 / 22 knock-downs (0x13 / 0x14); 19 .. 22 are left with Stand Up (7). 23 .. 36 mirror 0 .. 14 for trigger 15
(the immobilised stance, sent when a critical takes Movement to 1). Each transition's f32 is the playback rate
(keys a second) of the state it enters.

**Locomotion triggers** (`FUN_10064700`): with |throttle| > 0.05, |v| > 0.01 vmax, a sidestep or any turn → 2 if
|v| ≤ +0x10b4 else 3; otherwise 1 once. `FUN_10064d20` per state: cycles play at speed / stride (`FUN_100344e0`,
inverse root travel per loop, 8 when it is ≤ 0.125 m); +0x124a (moving) set for locomotion / posture-walk / jump /
skate states, clear for stand, crouch, prone and the rest; +0x1249 set for the posture walks (5, 6); skating moves by
velocity only (no root motion) and leans by the turn rate (+0x13d8, ×0.93 decay, ±0.5).

**Physics** (`FUN_1003af10`, step ≤ 0.1 s, forces × +0xd98 = 1/mass): gravity 20 (DAT_10163ae8, `FUN_1003d1f0`, always);
drive (20 + 2m) × throttle (+0xf58, `FUN_1003c0b0`); resistance (`FUN_1003c5f0`, ground only) (20 + 2m)(r|r| + 2r
without throttle), r = v / vmax × 4 with +0x1249, × 10 without +0x124a, velocity scaled by 1/r past r = 2; sideways
(400 + 30m) v_side / vmax (`FUN_1003c750`); traction cap 2g (`FUN_1003c8f0`). Yaw: torque (20 + 4m) × turn command
(+0xf50), damping 10 ω (+0x1010, `FUN_1003d390`) and, with no turn input, another 3 ω (`FUN_1003c750`, 30 × −0.1).
Slope force (`FUN_1003cc30`) only for the player's gear (+0x1f0); AI gears get none. Ground contact: snapped when the
body is at or under the ground; the grounded flag +0xf78 clears more than 2 m above it (player: 0.5 m). Landing faster
than 40 m/s down (+0x1080 = 2g) damages the gear in proportion up to 80 m/s (+0x1088 = 4g); the damage value is
behind an ftol the decompiler lost.
Jump (`FUN_10064d20` states 13 / 16): thrust timer +0x1070 = 0.45 s (+0x1068) × 0.5 for Jump, the whole 0.45 s for
Jump Jets (with the JUMP_JET effect, `FUN_10065b00(0)`); while it runs a force (0, 3.5 g, −0.7 g) in the body frame
(+0x1060 = 3.5): 70 up, 14 forward.

**Criticals, gears** (`FUN_10063460` vtable +0x8c → `FUN_100618a0` → `FUN_10062430`). Damage word = D << 14 | flags;
the low nibble is the hit direction (`FUN_100f3630`: within 60° of the fore-aft axis 1 / 2, else sides 8 / 4).
D ≥ 6A destroyed (0x100000); D ≥ 3A heavy (0x40000000; single player: direction 1 / 2 knocks the gear over, ECY
0x11 / 0x12); D ≥ 2A medium (0x20000000); D ≥ 0.2A light (0x10000000). Medium / heavy pick a system group
1 << round(rand % 4000 / 1000) (heavy: % 5000), applied only if the caller's mask has it (bullets and explosions 0xe,
`FUN_100f3430`; haywire 0x35, `FUN_100f3da0`, which also sets flag 0x40). Group tables (roll = round(rand % 6000 /
1000), −1 for medium, + the Exposed flaw):
- 1 fire control (flaw 20): 0-1 FC −1 (0x800); 2 FC −2/−3 (0x1000); 3-4 weapon destroyed (0x2000; Shielded Weapons
  perk 31 absorbs once); 5 FC −2 (0x4000; Backup Fire Control perk 3 absorbs once); 6 cockpit (0x8000).
- 2 movement (Reinforced Chassis flaw-word bit 8 absorbs once; flaw 23): 0-1 Movement −1 (0x80); 2 Maneuver −1
  (0x10000); 3 Maneuver −1/−2 (0x20000); 4 Movement halved (0x100); 5 knocked down (0x80000); 6 destroyed.
- 4 crew: medium: flaw 21 → knocked down; Reinforced Crew Compartment (flaw-word 9) / Emergency Medical (perk 16)
  absorb; else 0x40 (cockpit shake). Heavy: flaw-word 9 absorbs, else knocked down.
- 8 power (Rugged Movement System perk 30 absorbs once; flaw 22): medium Movement −1 (halved with the flaw); heavy
  Movement halved (Movement 1 with the flaw).
- 0x10: medium 0x400 (`FUN_10062180`: ECM / ECCM / jump jets / sensors / stealth down a level), heavy 0x200
  (`FUN_100622b0`: one of ECM, ECCM, life support, jump jets, sensors, stealth, comms lost entirely).
Effects (`FUN_10062430`): Movement words min 1 (at 1 the gear takes trigger 15); Maneuver +0xbf4; fire control
+0xbe8 (min −5, accuracy +0xb90 = clamp((FC + 5) / 8, 0.05, 1)); 0x8000: Backup Life Support (perk 0) absorbs, else
(rand & 1, or rand & 2 with flaw 24) destroyed or Movement 1; 0x80000: ECY 0x13 / 0x14 (rand & 1) once, single player;
armour (+0xbec, Armor × 10) −3 / −8 / −25 for light / medium / heavy. Haywire hits (flag 0x40): unless Haywire
Resistant (perk 17) a 5 s timer (+0xc8c) and a second critical roll, a third with flaw 27; the damage itself is not
reduced. Non-gear units (`FUN_10058fd0` / `FUN_10059240`): one table (Maneuver −1 / −2, FC −1 / −2, weapon,
destroyed ×2) and armour −5 / −10 / −20.

**Weapon explosions**: a round with a Collateral_Radius explodes (`FUN_100f3140`); `FUN_100f3430` gives every other
unit or structure inside the radius the same damage word, delayed by distance / 60 s.
