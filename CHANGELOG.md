# Changelog

Heavy Gear II native port - https://github.com/BatMahn/HeavyGear2-Port

Each release's source and the macOS / Linux / Windows builds are on the GitHub releases page. No game data is ever
included: you need your own copy of Heavy Gear II (see the README's "Setting up the game folder").

## 0.27.0 - 2026-11-20

### Weapons
- Trails, explosions and sparks come from the original game's effect tables (`FUN_100e4ca0`), picked by each
  weapon's ammo class:
  - Rockets, bazookas, panzerfausts, mortars, grenade launchers, incendiaries and guided missiles each get their own
    trail, trail colour, ground explosion and object explosion.
  - Every explosion plays the original's explosion sound.
- Bullets, lasers and rail guns spark by the ground surface they hit (its dust type and colour), with separate sparks
  for objects and under water.
- Muzzle flashes are picked per weapon.
- Spark-type particles that drew as large white blobs are now the right size.

### Missions
- The map's list of ground surfaces is read in full again: it stopped partway, so D01's lava did no damage.
- The user's gear walks its scripted path during intros (S1J's opening camera).
- FollowPath's six forms match the original.
- Scripted vehicles follow their routes:
  - dropships and aircraft (S1J, S01, S27, H09, S30)
  - maglev trains and elevators (M10, M20, S21, S22)
  - the H07 escort gear
- Building models whose names are too long now load (BPG20005 and others).
- The part-destroyed sound uses a file that exists.
- All 84 missions were run with full logging. What's left is missing from the game's own data: T05's and S27's
  missing paths, and one missing flare effect.

### Movies
- CRT filter:
  - curved screen, rounded tube corners and a thin bezel
  - scanlines that thicken with brightness, and a soft glow on bright areas
  - a faint aperture-grille stripe
- Interlaced filter: shows fields at twice the movie's frame rate, with combing on motion.

### Fixes
- Foot positions are measured at the sole, so footstep dust lands under the foot.

## 0.26.0 - 2026-11-12

### Cut-scenes
- Scripted cameras send the right "finished" and "interrupted" signals. The two were swapped, so S1J ran its skip
  branch and never left cut-scene mode; the letterbox and hidden crosshair now clear when the intro ends.
- A finished camera holds the view until the script or the view key switches away.
- Cut-scene mode ends only on ControlCamera(), SetCinematicControl(0) or a view switch, as in the original.
- The AI holds fire for the whole cut-scene unless the mission allows combat.

### Turrets
- SetWeaponConstraints sets both pitch and yaw limits, with the original's sign. Left and right were mirrored before.
- Turrets default to a ±30° gun pitch.
- Turret guns tilt toward their target and won't fire outside their arc.

### Movement
- Gears move by their walk/run cycle's own travel, played at speed ÷ stride, as the original does (`FUN_10064d20`).
- Walk or run is chosen by speed and switches at the animation's sync points, with a short blend.
- Ground physics use the original's numbers:
  - drive force and forward/sideways resistance from the gear's Maneuverability
  - a traction cap
  - a slope force: gears can't climb past about 30° and slide off faces steeper than about 37°
- Each terrain cell is split along its own diagonal (a bit in the terrain data), for height, slope and the drawn
  ground. This removes the sideways shove on the H01 dunes.

### Scripts and effects
- Artillery strikes as in the original: shells fall from 150 m and burst for 200 damage within 30 m, one every half
  second.
- SetSmoking puts light or heavy smoke on a unit.
- Wind blows particles, with gusts. The original starts every mission with a light wind.
- Idle gears sweep their torso while they have no target (AutoScan).

### Objects
- Spawn markers and light icons aren't drawn, as in the original.
- Jeeps and hovertanks no longer block movement. Shots still hit them.

## 0.25.0 - 2026-11-12

### Scripts
- Every scripted command that any mission calls is now bound. New ones:
  - Kill, FullStop, GetUnitDistance and Position
  - Teleport onto another unit
  - ArtilleryStrike
  - Persistent: scripts on dead units stop, unless marked Persistent
  - cut-scene control and AllowCinematicCombat
  - AutoIgnore / AutoAbort, AutoAim and SetWeaponConstraints

### Collision
- Collision is built from where the drawn geometry actually is: columns of boxes that follow overhangs, arches,
  bridge decks and the S2J dropship.
- Trees block only at the trunk.
- Objects the original marks as non-solid, editor markers and light icons don't block. This removes the invisible
  walls.

### Slopes
- Route planning uses the original's limits: 30° for most units, 20° for jeeps.
- Units avoid water deeper than their own size, unless amphibious.
- Thin seam lines along terrain tile edges are gone.

### Tools
- `hg2extract collaudit` reports suspicious collision across all missions.
- `HG2_COLLVIEW=1` draws the collision boxes in red.

## 0.24.3 - 2026-11-11
- The system mouse pointer is hidden; the game draws its own.
- Depth sorting is accurate out to the horizon, and ground under the water is drawn a little lower, so shorelines no
  longer shimmer.
- Trees collide with their trunk and crown instead of one 19 m-wide box.
- Footstep dust is a small puff at each foot and fades near the camera.

## 0.24.2 - 2026-11-11
- Walk and run cycles loop over their real length. A padding frame held the pose for ten frames, a stop in every
  stride.
- Cracks between fine (5×5) and coarse (3×3) terrain tiles are closed.

## 0.24.1 - 2026-11-11
- The walk cycle's built-in forward travel is taken out of the pose, and cycles play at the speed their stride gives,
  so feet plant instead of sliding.
- Gears stand on the highest ground under their feet; the ground height follows the drawn terrain.
- The mission-start title comes from the briefing (no more "none").

## 0.24.0 - 2026-11-11
- Rain falls as thin, faint, motion-blurred streaks.
- Options → Graphics:
  - anti-aliasing (off / 2× / 4× / 8× MSAA)
  - movie filter (off / CRT / interlaced)
  - movie screen (the original 4:3 frame, or fill the window)
- At first start the game asks for its folder with a folder picker and remembers it.

## 0.23.0 - 2026-11-11
- Route-request flags decoded for the pathfinder.
- Life Support is confirmed to do nothing in combat in the original; it only counts toward the threat value.

## 0.22.0 - 2026-11-11
- Route planning follows the original pathfinder:
  - a 4 m grid
  - squared step costs with height and water penalties
  - a 2048-node budget, with a partial path when there's no full route
- The build workflow has a "Run workflow" button.

## 0.21.1 - 2026-11-11
- The README covers setting up the game folder from the CD, with no installer.
- New repository: HeavyGear2-Port.
- CI: macOS builds on macos-15, and packaging no longer tries to install the bundled libraries.

## 0.21.0 - 2026-11-11
- Path patrols in combat.
- The free-range radius is used.
- Hold and Go To use the original's guard rule.
- Moves give up after 5 s without progress.
- Patrolling units stay awake out of sight.

## 0.20.0 - 2026-11-11
- Squad mates use the original's follow and defend rules.
- AssignPath paths are patrolled the original's way (PathFollow1/2).
- Units plan routes around obstacles.

## 0.19.0 - 2026-11-11
- The original AI tactics, decoded from the game:
  - the combat tactics, flanks, hand-to-hand, turret and tank attacks
  - run away, hunt, patrol, pursue and hold ground

## 0.18.0 - 2026-11-11
- Concealment from posture (decoded): kneeling, going prone and holding fire make you harder to spot.
- HEP, backup life support and backup comms perks.

## 0.17.0 - 2026-11-11
- AI difficulty follows the campaign pilot's setting.
- Gears skate on long runs and kneel to shoot.
- Ejection, zero-G movement and the Easy to Modify perk.
- Low gravity in space.

## 0.16.0 - 2026-11-11
- Squad commands (F5–F10) and a target bracket on screen.
- Guided missiles, with ECM / ECCM locks.
- Sniper, designator, uplink, amphibious and backup-system perks.

## 0.15.0 - 2026-11-11
- Gear systems in combat:
  - manoeuvre turn rate
  - sensors, stealth, ECM and ECCM
  - jump jets
  - armour perks and flaws, and critical-hit backups
- AI: chooses a weapon by range, checks its line of fire, gets unstuck and avoids lava.

## 0.14.0 - 2026-11-11
- Network keep-alive thread, so a stalled frame doesn't drop players.
- Windows build checked: cross-compiled with MinGW and run under Wine.

## 0.13.0 - 2026-11-11
- Multiplayer over the port's own UDP protocol.
- Gear lab: structure ratings, and perks & flaws editing.

## 0.12.0 - 2026-11-11
- Threat values and mission threat limits.
- Variant stats, perks and flaws.
- Flank and circle tactics.

## 0.11.0 - 2026-11-11
- Movies through a bundled Indeo 5 decoder (from FFmpeg, LGPL).
- Gear lab weapon refits.
- Terrain surfaces, with lava damage.
- Hit reactions.
- Particle emitter curves.

## 0.10.0 - 2026-11-11
- The original front end (shell) and the single-player campaign:
  - briefings, squad and gear choice, debriefings
  - historical and training missions, instant action

## 0.9.0 - 2026-11-10
- The original damage model, unit generators, weather, lights and music.
- Direct3D 12 shaders, and release builds for all three platforms.

## 0.8.0 - 2026-11-10
- Per-part damage.
- Weapons fire from the hardpoints' muzzles.
- Pilot's-eye cockpit view.

## 0.7.0 - 2026-11-10
- Unit brains and events.
- Every scripted command bound.
- Particles.
- Terrain axis fix: objects sit on the ground.

## 0.6.0 - 2026-11-10
- Objectives, nav points, postures, the cockpit view and camera flights.

## 0.5.0 - 2026-11-10
- Scripted unit orders and mission paths.
- Audio and the HUD.

## 0.4.0 - 2026-11-10
- Object collision.
- Weapons from MstrWpns.bin, projectiles and damage.
- A first, provisional AI.

## 0.3.0 - 2026-11-10
- A drivable player gear.
- Gear definitions and the animation state machine (ECY format).

## 0.2.0 - 2026-11-10
- Mission scripts run live in the game loop.

## 0.1.0 - 2026-11-10
- Mission viewer: archives, terrain, models and animated gears on an SDL_GPU renderer.
