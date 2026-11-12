# Heavy Gear II - native port

https://github.com/BatMahn/HeavyGear2-Port - by [BatMahn](https://github.com/BatMahn)

A native port of **Heavy Gear II** (Activision, 1999) for modern macOS, Linux and Windows, rebuilt from the original
`hg2sim.dll` / `shell.dll` by reverse engineering. C++17 with SDL3 (GPU API: Metal, Vulkan, Direct3D 12).

No game data is included. You need your own copy of Heavy Gear II (see "Setting up the game folder": no
installation needed).

## Status (v0.24.1) — the whole single-player game

The game now starts in the original shell, drawn from the game's own layouts and artwork:

- **Main menu**, single-player menu, **game options** (sound volumes, special effects / window mode, controls and
  mouse settings) and the **credits** roll.
- **Campaign**: register a pilot (name and one of the Black Talon chassis), new game / continue / load game, mission
  briefings with the SATCOM video (press play), **squad detail** (pick your squad mates
  from the pilots the mission allows, with their bios; pilots killed in action stay dead), **modify gear** (your
  chassis' variants, and refit any hardpoint with a weapon stock gears carry there), launch, **debriefing** (the mission's success / failure / alternate texts,
  kills, accuracy, casualties, time) and on to the next mission by the briefings' own mission order. Autosaves at
  every launch and return, as the original did.
- **Historical** missions and **training** missions, each with their briefings; historical missions let you choose any
  chassis and variant and a squad of four.
- **Instant action**: your gear and up to four squad mates against up to five enemy gears, 1–10 waves, any of the
  15 arenas.
- Squad mates fly in formation behind you and fight what comes close; scripts address them as the mission's squad.
- `Esc` in a mission pauses: resume, abort (back to the debriefing) or quit.
- Settings and pilots are saved in your user folder (`~/.local/share/BatMahn/HG2Port` on Linux,
  `~/Library/Application Support/BatMahn/HG2Port` on macOS, `%APPDATA%\BatMahn\HG2Port` on Windows).

- **Movies**: the title movie at start-up, the campaign's cut-scenes (intro, gate, flight, leaving, asteroid, outro)
  where the missions call for them, and the briefing videos — all Indeo 5, decoded by a copy of FFmpeg's Indeo 5
  decoder (`third_party/indeo5`, LGPL 2.1 or later). Any key or click skips; `--no-movies` turns them off. The
  full-screen movies are looked for in a `movies` folder next to the data folder (the CD's `Setup\movies`).

- **Threat values**: the gear lab shows each gear's threat (total, offence, defence, miscellaneous) computed the way
  `shell.dll` does it from the game's construction tables, and launch is refused when a gear or the squad goes over
  the mission's limits. Variants carry their own armour and fire control into the mission; the notes list each
  variant's perks and flaws.

- **Gear lab structure and perks & flaws**: armour, sensors, fire control and manoeuvre can be stepped up or down,
  and any perk or flaw switched on, off or to another level; the threat value follows, and so does the fight:
  manoeuvre sets the turn rate (0.6 + 0.05 × rating, as the original) and makes the gear harder to hit; sensors
  stretch the radar and the range enemies are seen at; stealth shrinks the range the gear is seen at; ECM throws off
  enemy fire unless the shooter has ECCM; jump jets jump higher; Reinforced Armor, Heat Resistant Armor (lava) and
  Haywire Resistant cut damage, Weak Facing adds to it; Backup Fire Control, Reinforced Chassis, Rugged Movement and
  Reinforced Crew Compartment each soak up one critical hit, exposed-system flaws make criticals worse, and heavy
  criticals knock ECM, ECCM, jump jets or stealth down a level. Mission gears get their perks and flaws from their
  own definitions too.
- **Squad commands** (the original's six, F5–F10): attack my target, defend my target, hold ground, regroup, fire at
  will, go to the selected nav point. "My target" is the hostile nearest the crosshair, bracketed on the HUD.
- **More perks in the fight**: guided missiles (AAM, AGM, ATM, HATM) home on the target you're aiming at, and a
  target's ECM can break the lock unless the shooter's ECCM holds it; Sniper Systems steadies aim and adds a zoom
  (V); Target Designator steadies your whole side against your target; Satellite Uplink widens the radar; Amphibious
  gears don't slow down wading through deep water; Backup Sensors keep the sensors working after a sensor hit;
  Shielded Weapons and Emergency Medical soak up a critical hit; squad mates with an Ejection System survive losing
  their gear (they're not lost to the campaign); Zero-G Movement keeps full speed on space missions (which now have
  low gravity); Easy To Modify lets the gear lab put any weapon on any hardpoint; hostile-environment protection halves
  lava damage; Backup Life Support soaks up a crew critical; without a Backup Communication System, squad commands
  fail once the sensor head is shot off.
- **Concealment** (decoded from the gear's posture handling, `FUN_10064d20`): standing or moving 0.15, kneeling 0.25,
  prone 0.4, plus 0.05 per Stealth level, minus 0.15 for a Large Sensors Profile, halved for two seconds after
  firing. It sets how far away a gear is spotted, so kneeling or going prone (and holding fire) now hides you.
- **AI**: the campaign pilot's difficulty now counts (how far hostiles see you and how well they shoot); gears skate
  on long runs out of a fight and kneel to shoot when holding ground (scripted stances and paths are left alone);
  units pick the weapon for the distance (melee up close, otherwise the hardest hitter in reach), hold at the
  selected weapon's range as the original's standard combat tactic asks, won't shoot into a hill or through a friend
  (they move to get a clear line instead), back off and turn when stuck, and walk out of lava (waves no longer spawn
  in it, and units never doze while standing in it).
- **Original AI tactics** (0.19): units now run the original tactic objects' state machines, decoded from
  `hg2sim.dll` — GearCombat1/2/3, the flank family, hand-to-hand, stand-and-shoot, turret and tank attacks,
  run-away, HuntUser/HuntTarget re-planning, patrol / free-range wandering round home, pursue and hold-ground —
  with the tactic table's per-class choice when a script doesn't name one, and the original order kinds (MOVETO,
  TURNTO, STAND, FIREWPN volleys) executed by the gear.
- **AI, 0.20**: squad mates use the original SquadMateFollow / Defend / Guard rules, `AssignPath` paths are walked
  the way PathFollow1/2 do (up and down the path at random, or round it in turn), and units plan a route round
  buildings, cliffs and lava when the straight line is blocked or they get stuck.
- **AI, 0.21**: PathFollow2 units keep to their path in a fight (shooting on the move, closing node by node);
  `ActivateFreeRange` radii are used; Hold / Go To use the original guard rule; moves give up after 5 s without
  progress like the original's path follower; patrolling units no longer doze off out of sight; braked units
  don't re-plan.
- **AI, 0.22**: route planning now follows the original pathfinder: a 4 m grid, squared step costs with height and
  water penalties, a 2048-node budget with partial paths to the nearest reachable point, 2 m straight-line checks.
  The build workflow has a "Run workflow" button.
- **0.24.1**: the walk / run cycles' root motion (the body travels ~7 m through each cycle and snapped back) is now
  taken out of the pose, and the cycles play at the speed their stride gives, so feet plant instead of sliding; gears
  stand on the highest ground under their feet (and the ground follows the drawn terrain triangles), so they no
  longer sink into hillsides; the start-of-mission title uses the briefing's title (no more "none").
- **0.24**: rain drawn as thin motion-blurred streaks (denser, fainter, fading at the tail); video options in
  Options → Graphics: anti-aliasing (off / 2x / 4x / 8x MSAA), a movie filter (CRT scanlines and tube edges, or
  interlaced fields) and the movie screen (the original 4:3 frame, or filling the window); the game folder is picked
  with a folder dialog at first start.
- **0.23**: MOVETO's request flags decoded (gears wade anywhere; path nodes sit on the water surface); Life Support
  confirmed to have no effect in the original simulation (it only counts toward the threat value). The Windows build
  was cross-compiled again and runs a D01 skirmish under Wine.
- **Multiplayer**: free-for-all matches on the instant-action arenas over LAN or the internet, with the port's own
  UDP protocol (the original's ANet / DirectPlay sessions can't be reached any more). Main menu → Multiplayer: one
  player hosts (UDP port 47250; forward it for internet play), the others join by address; each brings the gear set
  up on that screen (Modify Gear). Frag and time limits, respawns, a frag table in the HUD. From the command line:
  `hg2 --host 2 --mission D01` and `hg2 --join <address> --gear "Black Mamba"`. Every machine needs the same game
  data. Each player's own machine decides their armour (it sees the other players' shots), so a laggy link means
  dodging on your screen counts.

Engine fixes that came with the campaign: the script VM evaluated non-commutative operators (`-`, `/`, `<`, `>`, …)
with their operands swapped, which flipped countdowns and checks such as `UserAlive() > 0`; teams now start relaxed
(defcon 0) and grow alert from what they see and suffer, hold fire until then, units off-screen and over 300 m away
doze (`AutoSleep`), `FullBrake` holds a unit until it's ordered or attacked, and `HuntUser` units go for you.

What works in the missions:

- Reads the original data directly: `simpack.prj`/`hg2shell.prj` archives, LZB-compressed
  `Terrain\*.MR~` terrains (so a CD's `Setup\data` folder works without installing), `.M` models,
  `.XB` animation libraries, BMP/TGA textures and compiled `.ANN` missions.
- Renders any mission: terrain with its textures, sky dome, lava/water plane, fog and lighting
  from the mission, and every placed object. Gears are posed and animated from their `.XB` sets.
- SDL3 GPU renderer: Vulkan (Linux, Windows) and Metal (macOS).
- Your gear: the mission's player gear (from its definition: models, `.ECY`/`.XB` animations, walk/run speeds)
  is driven through the original `.ECY` animation state machine: walk, run, sidestep, kneel, go prone, crawl,
  jump (take-off / airborne / landing) and SMS skates. Cockpit (internal) and chase (external) views.
- Gears stand on top of objects (blocks, platforms) as well as terrain; the chase camera stays out of hills.
- Collision with buildings and other static objects (per-part boxes from the models).
- Weapons: all 200 entries of `MstrWpns.bin` decoded (fire rate, velocity, range, damage, ammo, sounds,
  models). Your loadout comes from the gear's hardpoints; projectiles and hitscan beams hit terrain,
  objects and units; impacts flash; units and vehicles can be destroyed.
- Unit AI driven by the mission scripts' brain settings: enemy / friendly / focus notify ranges, visibility,
  team alert level (defcon 0–5), `HuntUser` and other tactics, `CanTarget`, posture (`SetStandingMode`), skates,
  `Forward`/`Backward`, `PausePath`, `LookAt`, `Teleport`, `InstantDeath`, `AutoPilot`. Escort vehicles keep to their
  path and shoot on the move. Fighting follows the original tactic objects (0.19): see `docs/FORMATS.md`.
- The engine's unit events reach the scripts: damaged (2), destroyed (5), hit (11), hit by you (12), path end (13),
  enemy in/out of range (15/17), friend in/out (19/21), focus in/out (23/25), combat start/end (29/30).
  Every builtin any mission calls is now bound (weather, lights and fountains are accepted but not drawn yet).
- Particle effects from the game's own `Particle System` definitions (`.ped`/`.emt`/`.ptl`): weapon impacts,
  muzzle flashes, unit and turret explosions, foot dust.
- Screen fades (`BeginFade`), `SetWaterLevel`.
- Per-part damage from each gear's `Damage_Hierarchy`: hits land on the nearest part (head/sensors, torso, arms, legs)
  at the current pose. Arms come off with the weapons mounted on them, leg damage slows the gear, losing the sensor
  head blinds the radar (and halves an AI's detection range), and breaking the torso structure brings the gear down.
  Damaged gears smoke. Your HUD shows a damage diagram and warnings.
- Weapons fire from their hardpoint frames (`frm-hpN`) and the cockpit camera sits on the model's `frm-POV` eye frame.
- The original damage model (`FUN_10058fd0`/`FUN_10059240`): armour (Armor ×10) wears down 5/10/20 per light /
  medium / heavy hit, judged against the hit's damage (weapon Damages ×10 × the shooter's fire-control multiplier);
  medium and heavy hits roll criticals (drive, gyro, a weapon knocked out, or the gear destroyed), and a hit of six
  times the remaining armour destroys it. Buildings use their structure points; blast weapons hurt everything in their
  Collateral_Radius. Scripts get the original damage events 2/3/4. Damaged parts show the gear's damaged skin.
- Destroyed gears fall (the `.ECY` fall animations) and stay as wrecks; gears step when turning on the spot and the
  torso twists toward your aim.
- Unit generators (mission `FountainSystem` blocks) send reinforcements, `ActivateFountain` switches them;
  mission lights glow and `ActivateLight` switches them; `SetWeatherType`/`SetWeather` bring rain or snow.
- AI combat tactics: stand-and-shoot, drone rushes, hand-to-hand closing, gears circling at range, retreat when
  badly hurt, aim with lead and fire-control scatter.
- Sound objects from `Sounds.lst` (volumes, ranges, loops); buildings with looping sounds hum when you're near.
- Music: put your ripped CD tracks (`.mp3` or `.wav`) in a `music` folder inside the data folder (or pass
  `--music <folder>`); they play shuffled. `M` toggles music.
- Mission scripts now command units: `FollowPath`/`AssignPath` (mission paths, incl. Bezier segments, are decoded),
  `AutoSleep`, `ActivateUnit`, `SetSpeed`, `Attack`, `FullBrake`, `SetEnemyNotifyRange`, `SetUserControl`, `AutoBreak`.
- Sound: weapon fire, hits and explosions (positional), mission sounds and queued dialog from the game's WAVs.
- Mission UI from the scripts: objectives (`SetUserObjective`, with the mission's message file), nav checkpoints
  (`SetUserCheckpoint`, Select/Next/Prev/Kill) on the HUD and radar, centre-screen instructions (`RegisterString`),
  scripted camera flights (`ControlCamera`, letterboxed, `Space` skips), focus range triggers (`SetUnitFocus`),
  `GetUnitStatus` posture/view queries, `LockForwardStick`, `FlushDialog`, `SetHitpoints`, `Invincible`.
- The training mission T01 (Movement Systems Training) plays through its lessons: views, camera, postures, nav points.
- HUD: armour bar, weapon/ammo, speed, posture, view, aim reticle, 300 m radar with contacts and the selected nav point,
  objectives list, mission title.
- Footsteps from the gears' step sounds and a looping engine sound for your gear.
- Free-fly camera (F1 toggles).
- Mission script VM: the original compiled scripts (`Scripting/*.o`) load and run every tick — full instruction set,
  cross-script calls, events, timers, signals, and the math/vector/state/timer/unit-query builtins.
  `--verbose-scripts` logs each builtin a mission calls that isn't implemented yet.

Missions run start to finish where they rely on what's above; combat missions still use the provisional AI.
See the roadmap below.

## Building

### CMake (macOS, Linux, Windows)

```sh
cmake -B out -DCMAKE_BUILD_TYPE=Release -DHG2_FETCH_DEPS=ON   # downloads and builds SDL3 + zlib
cmake --build out --config Release
```

macOS builds a `Heavy Gear II.app`. Windows uses Direct3D 12 (DXIL shaders are built in) or Vulkan; macOS uses
Metal; Linux uses Vulkan. `.github/workflows/release.yml` builds all three on GitHub and attaches zips to a release
when you push a `v*` tag.

### Makefile (quick local builds)

Needs a C++17 compiler, SDL3 (3.2+), zlib and pkg-config.

```sh
# macOS
brew install sdl3 pkg-config
# Fedora
sudo dnf install SDL3-devel zlib-devel pkgconf
# Debian/Ubuntu (24.10+)
sudo apt install libsdl3-dev zlib1g-dev pkg-config

make hg2      # the game
make tools    # hg2extract (asset inspection)
```

Shaders are pre-compiled into `shaders/gen/` (SPIR-V, MSL and DXIL). To rebuild them after editing
`shaders/*.vert|frag`, install `glslangValidator` and `spirv-cross` (plus `dxc` for DXIL, `DXC=/path/to/dxc`) and run
`make shaders`.

## Setting up the game folder

The port needs the original game's files in one folder (called `HeavyGear2` below; any name and place will do).
**You don't need to install the game.** Heavy Gear II's installer is a 1999 InstallShield setup for Windows 95/98 that
expects DirectX 6; you can skip it. Everything the port reads is on the CD as plain files, and you copy them over by
hand.

    HeavyGear2/
      simpack.prj     REQUIRED  (CD: Setup\data\, 208 MB) the simulation archive: models, terrain, gears, sounds
      simpack.map     REQUIRED  (CD: Setup\data\) its index
      hg2shell.prj    REQUIRED  (CD: Setup\data\) the shell's databases (pilots, missions, weapons, perks)
      hg2shell.map    REQUIRED  (CD: Setup\data\)
      Mission\        REQUIRED  (CD: Setup\data\Mission\) the mission files
      Scripting\      REQUIRED  (CD: Setup\data\Scripting\) the compiled mission scripts
      Shell\          optional  (CD: Setup\data\Shell\movies\) the briefings' SATCOM movies
      Terrain\  Objects\  Textures\  Audio\  Particle System\
                      REQUIRED  (CD: Setup\data\) loose terrain, objects and effects
      MstrGear.bin  MstrWpns.bin  Sounds.lst  where
                      REQUIRED  (CD: Setup\data\) gear and weapon tables, sound list, search paths
      EUM_____.TTF  EUB_____.TTF  s017016d.ttf
                      REQUIRED  (CD: Setup\data\) the shell's fonts
      zip\zfs.zip     REQUIRED  (CD: Setup\zip\zfs.zip, 12 MB) the shell artwork the installer would have unpacked
      movies\         optional  (CD: Setup\movies\, 61 MB) the title movie and the campaign's cut-scenes
      music\          optional  the soundtrack as .mp3 or .wav (see "The music")

The simplest way: copy **all of `Setup\data\`** (293 MB) into `HeavyGear2`, then add `Setup\zip\` and
`Setup\movies\` as the `zip` and `movies` folders beside it. The programs and DLLs that come along (`Heavy Gear
2.exe`, `hg2sim.dll`, `shell.dll`, `anet2.dll` ...) aren't used, but they're harmless. File names are matched without
regard to case.

The port keeps its settings, pilots and saved games in your user folder (`~/Library/Application Support/BatMahn/HG2Port`
on macOS, `~/.local/share/BatMahn/HG2Port` on Linux, `%APPDATA%\BatMahn\HG2Port` on Windows), so the game folder
can be read-only.

### What's on the CD

The Heavy Gear II CD has `Setup\` (the installer and the game), `DIRECTX\` and `HELP\`. Inside
`Setup\`:

- `data\` is the game exactly as the installer lays it out on your hard drive. `data_e`, `data_f95` ... `data_s98`
  are the installer's language and Windows-version extras: not needed.
- `zip\zfs.zip` holds the shell's artwork (`Shell\shellart\*.zfs` in an installed game). The port reads it
  straight out of the zip; you don't need to unpack it.
- `movies\` holds the full-screen movies (`title`, `intro`, `gate`, `flight`, `leaving`, `asteroid`, `outro`).
  The mission briefings' SATCOM movies are in `data\Shell\movies\` and come along with `data`.
- Everything else (`setup.exe`, `*.ldl`, `DXMEDIA.exe`, `DIRECTX\`, `HELP\`) can be ignored.

### Copying the files (no installer)

Put the CD in the drive (or mount an image of it) and copy:

- **Windows:** with the CD in drive D:, in a Command Prompt:

      xcopy "D:\Setup\data\*.*" C:\Games\HeavyGear2\ /E /I
      xcopy "D:\Setup\zip\*.*" C:\Games\HeavyGear2\zip\ /E /I
      xcopy "D:\Setup\movies\*.*" C:\Games\HeavyGear2\movies\ /E /I

  (or do the same in File Explorer: the contents of `Setup\data` into `C:\Games\HeavyGear2`, then `Setup\zip` and
  `Setup\movies` into it as folders).
- **macOS:** the CD mounts under `/Volumes` (check its name with `ls /Volumes`; `HG2` below). In Terminal:

      mkdir -p ~/HeavyGear2
      cp -R /Volumes/HG2/Setup/data/ ~/HeavyGear2/
      cp -R /Volumes/HG2/Setup/zip ~/HeavyGear2/
      cp -R /Volumes/HG2/Setup/movies ~/HeavyGear2/

- **Linux:** mount the CD (or `sudo mount -o loop hg2.iso /mnt`) and

      mkdir -p ~/HeavyGear2
      cp -r /mnt/Setup/data/. ~/HeavyGear2/
      cp -r /mnt/Setup/zip /mnt/Setup/movies ~/HeavyGear2/

  (a real disc mounts under `/run/media/$USER/` or `/media/$USER/` with the disc's name).

If you only have an image (`.iso`, `.cue` + `.bin`, `.mds` + `.mdf`), mount it (Windows Explorer and macOS mount
`.iso` files with a double-click; other formats need a tool such as WinCDEmu, or `bchunk` to convert them) and copy
from it the same way.

**An existing installation works too:** point the port at the installed game's folder (the one with
`simpack.prj`; usually `C:\Program Files\Activision\Heavy Gear II`). It has the shell art already unpacked in
`Shell\shellart\`; its `movies` folder is found next to it. **Running straight from the CD** also works:
`--data D:\Setup\data` finds `Setup\zip` and `Setup\movies` beside it.

### The music

The port plays your own rip of the soundtrack: put the tracks (`.mp3` or `.wav`, 16-bit PCM) in a `music` folder
inside the game folder, or pass `--music <folder>`. They play in a shuffled order through missions; `M` toggles the
music. Without them the missions play with their sound effects only.

    HeavyGear2/music/01.mp3
    HeavyGear2/music/02.mp3
    ...

### The movies

The movies are Indeo 5 AVIs. The port has its own copy of FFmpeg's Indeo 5 decoder built in (`third_party/indeo5`),
so nothing else needs installing. Any key or click skips a movie; `--no-movies` turns them off.

### A finished folder

    HeavyGear2/
      simpack.prj  simpack.map  hg2shell.prj  hg2shell.map  MstrGear.bin  MstrWpns.bin  Sounds.lst  where
      EUM_____.TTF  EUB_____.TTF  s017016d.ttf
      Mission/  Scripting/  Shell/  Terrain/  Objects/  Textures/  Audio/  Particle System/
      zip/        zfs.zip
      movies/     title.avi  intro.avi  gate.avi  flight.avi  leaving.avi  asteroid.avi  outro.avi
      music/      your soundtrack rip (optional)
      (plus whatever else came along from Setup\data\: Heavy Gear 2.exe, the DLLs, Anet\ ... - harmless)

Then start the game. The first time, it asks for the game folder with your system's folder picker (pick
`HeavyGear2`, or the CD itself) and remembers it; to change it later, start with `--data <folder>` once. `--data`
and `HG2_DATA` still work and take precedence.

## Running

```sh
./build/hg2 --data ~/HeavyGear2
```

Without `--data` (or `HG2_DATA`) it looks for a `data` folder next to the program.

- `--mission NAME` — start any file in `Mission/` (D01…D13, M01…, H01…, S…, T…) directly, skipping the menus
- `--window 1600x1200`, `--fullscreen` (Alt-Enter toggles in the menus)
- `--shell SCREEN --screenshot out.png [--mouse x,y]` — render a shell screen offscreen (main, single, options,
  sound, graphics, controls, credits, pilots, load, missions:training, briefing:S01, squad:S01, gear:S01,
  debrief:S01:1, instant)
- `--squad N`, `--waves N` — squad mates / instant-action waves on a `--mission` run
- `--screenshot out.png [--cam x,y,z,yaw,pitch] [--size 1280x720] [--nofog]` — render one frame offscreen
- `--external` — start in the chase view (the default is the cockpit view, as in the original)
- Test helpers for offscreen runs: `--drive N` frames, `--nav-drive` (steer to the selected nav point),
  `--input 600:down,700:jump` (posture/jump/skates/view actions at given frames), `--side` (camera beside your gear);
  environment: `HG2_GOD=1` (invincible), `HG2_AIDBG=1` (unit dump), `HG2_FX="EXP_HG"` (effect ahead of you),
  `HG2_OBJY=1` (object-vs-terrain height check), `HG2_PATH=name` (path heights)
- `HG2_DATA` can be set instead of `--data`

Controls (the original numeric-pad layout works too, so the training messages match):

| Action | Keys |
|---|---|
| Throttle / turn | `W` `S` `A` `D` or pad `8` `2` `4` `6`; hold `Shift` to run |
| Sidestep | `Q` / `E` or pad `7` / `9` |
| Posture down / up (stand → kneel → prone) | `Z` / `X` or pad `/` / `*` |
| Jump | `Space` or pad `Enter` |
| SMS skates on/off (when stopped) | `R`, `Insert` or pad `0` |
| Internal / external view | `C` (the original's Ctrl-C) |
| Rotate / tilt camera, zoom | arrow keys, `Home` / `End` or mouse wheel; mouse also looks / aims |
| Fire, select weapon | left mouse or `Ctrl`; `1`–`8`, `Tab` |
| Next / previous nav point | `N` / `B` |
| Music on/off | `M` |
| Skip camera flight | `Space` |
| Free camera | `F1` (mouse look, `WASD`, `Space`/`Ctrl` up/down, `Shift` fast) |
| Squad commands | `F5` attack my target, `F6` defend my target, `F7` hold ground, `F8` regroup, `F9` fire at will, `F10` go to nav point |
| Sniper zoom | `V` (gears with Sniper Systems) |
| Pause menu (resume / abort / quit) | `Esc` |

`./build/hg2extract <datadir> scripts T01 60` runs a mission's scripts headless for 60 seconds and lists the
builtins it needed that aren't implemented yet.

`./build/hg2extract <datadir> check` parses every model, image, mission and terrain and reports failures.

## Roadmap

1. ~~Data layer: archives, terrain, models, animations, missions~~ (0.7 fixes the terrain axes: the `.MRM` grid's
   first axis is world Z — every mission's objects now sit on the ground, and Bezier mission paths are continuous)
2. ~~Renderer bring-up: terrain, sky, water, objects, posed gears~~
3. Definition files: ~~gear definitions (models, stats, hardpoints, sounds)~~; weapons, objects, `MstrGear.bin` / `MstrWpns.bin`
4. ~~`.ECY` animation state machine: locomotion, posture, jump, falls, turning on the spot, hit reactions~~
5. ~~Player gear: walk/run on terrain, chase camera, collision with objects, cockpit camera, HUD, torso twist~~
   (the original cockpit is the vector HUD; there is no cockpit bitmap art in the data)
6. ~~Weapons, projectiles, hits, particle effects, damage hierarchy, the original damage model and criticals,
   damaged skins, collateral damage, emitter rate / speed curves~~
7. ~~Mission script VM (`Scripting/*.o`), every builtin, unit events, brain settings, weather, lights, unit
   generators, terrain surfaces (lava damage), the tactic table and the original tactics' state machines~~
   (the MOVETO pathfinder's A* decoded in 0.22; the gears' slope / depth limits are estimates)
8. ~~Audio: WAV effects, dialog, footsteps, engine loop, EAR sound objects, music (your CD rips)~~
9. ~~Shell: menus, briefings, squad and gear selection, debriefings, campaign flow and saves, historical, training,
   instant action (`shell.dll`, `hg2shell.prj`, `*.zfs`), movies (Indeo 5), gear lab weapon refits, threat values and
   mission threat limits, structure and perks & flaws editing, multiplayer (the port's own protocol)~~
10. ~~Windows Direct3D 12 shaders (DXIL), release builds~~ (CI builds; the Windows build has been cross-compiled with
    MinGW and run under Wine with Vulkan — shell, missions and a network match against the Linux build — but not yet
    on real Windows or Mac hardware)

## Layout

- `src/core` — file system (archives, `where` search paths, LZB), binary readers, math
- `src/assets` — decoders for models, terrain, images, animations, missions
- `src/render` — SDL_GPU renderer
- `src/script` — mission script VM and builtins
- `src/game` — scene / game code; `session.cpp` runs one mission with the shell's choices
- `src/shell` — the shell: LDL layouts, UI, shell databases, screens and the campaign
- `docs/FORMATS.md` — reverse-engineering notes for every format, with the engine functions they come from
- `tools/` — `hg2extract`, plus Python reference decoders in `tools/re/`

## Third-party code

- `third_party/stb_truetype.h` — public domain / MIT (Sean Barrett).
- `third_party/indeo5/` — the Indeo 5 video decoder from FFmpeg n6.1, LGPL 2.1 or later
  (`third_party/indeo5/COPYING.LGPLv2.1`); it is built as its own static library (`indeo5`) and can be replaced.

## Legal

Heavy Gear is a trademark of Dream Pod 9; Heavy Gear II is (c) 1999 Activision. The game's data is theirs: this
repository ships none of it, and you need your own copy of the game.

## Credits

Port by BatMahn (https://github.com/BatMahn).
