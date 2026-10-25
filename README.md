# Bxsim

A real-time audio visualizer written in C++ with SFML 3.1.

Open a song and it plays while the visuals react to it, with bloom glow, beat-reactive pulses and particles. There are fourteen modes:

1. **Bars**: classic spectrum bars with peak caps and a floor reflection.
2. **Ring**: a mirrored circular spectrum that pulses on the kick.
3. **Scope**: a glowing waveform over a smooth spectrum silhouette.
4. **Lissajous**: left vs. right channel drawn as spinning, color-shifting loops.
5. **Tunnel**: a feedback kaleidoscope; the spectrum flies outward through a color-shifting tunnel.
6. **Liquid**: glossy, swirling liquid folded into a kaleidoscope; the bass bends it.
7. **Oscilloscope**: a green-phosphor CRT oscilloscope with a graticule. Tab switches between XY (left vs. right, for oscilloscope music) and a classic time sweep.
8. **Dark Fantasy**: a 3D fly-through of a medieval village at night, gliding down a wet cobblestone street toward a castle on the horizon. Timber and stone houses and spired towers rise with the spectrum, windows pulse brighter and dimmer with the music, street lamps glow through the fog, the moon casts real shadows, bats scatter on the beat and big kicks call down lightning. The bass pushes you forward faster.
9. **Pop**: bright, flat cartoon style with bold ink outlines. A sunset-purple sky with flat clouds, starbursts that pop on every kick, rings that ripple outward, falling confetti and sparkles, and a candy-colored spectrum arch.
10. **Night Drive** (key 0): a late-night highway drive from the driver's seat. Rain beads up on the curved windshield and drops break loose and run down it, leaving wet trails, streetlights rush past and light up the car, wet asphalt reflects every light, tail lights flash like brakes on the kick, oncoming headlights flare by, the city skyline's windows pulse with the music and the dashboard glows: the speedometer follows your speed (the bass pushes you faster), the tach follows the energy, and the car radio shows the song name (scrolling if it's long), the time and length, a progress bar and an equalizer. The steering wheel turns the way the road bends.
11. **Dark Forest** (key -): walking through dense woods at night with only a flashlight. Its beam cuts through the fog and lights up the trunks, pages are pinned to some trees, and static crackles and tears across the picture on the beat. The flashlight flickers out on big hits and the bass quickens your pace.
12. **Hijack** (key =): a hacked live feed in loud neon hacker-punk style. A flowing neon field printed in big halftone dots with spray-paint strokes across it, a terminal typing out fake hacks, retro popup windows that spam on the beat (errors, upload bars, a live signal meter, pixel-art pictures), die-cut pixel stickers, the spectrum as stacked pixel blocks, and huge slanted words like OVERRIDE that slam in on big kicks. The whole frame tears, splits into RGB and flashes inverted with the music, and the color scheme flips on big drops.
13. **Rage** (key [): in the crowd at a rage show. A dark, hazy venue with red and white beams sweeping from the stage truss and a giant LED wall, a packed crowd of weird, one-of-a-kind silhouettes jumping with the bass and backlit by the stage (spiky hair, swinging dreads, huge afros, mohawks, little horns, long necks, devil-horn hands, phones up recording, headbangers, someone crowd-surfing, and a few turned around grinning at you with glowing eyes), hard white strobes on the hits, camera shake and punch-ins, blown-out flashes on the biggest drops, bell sparkles on the high notes, heavy grain, and distorted umlaut words like RÄGE that slam in on drops.
14. **Dystopia** (key ]): a slow, heavy glide down a canyon of colossal black megacity towers in glowing toxic-green smog. Window lights switch on and off with the music (whole towers light up on loud parts), waves of light run up the towers on every kick, light strips pulse with the bass, red rooftop warning lights flash together on the kick, giant cold corporate screens hang on the towers, searchlights sweep the smog, and a colossal monolith stands on the horizon with a glowing core that throbs with the bass. The biggest hits flash the whole city, and a cold surveillance HUD watches over it all. Four color schemes: Toxic green, Neon shift (drifts through green, cyan and purple), Cyber (cyan and magenta) and Red.

### Add a picture

Drop an image on the window (or press **I**). It's fitted so the whole picture shows. For a few seconds it looks normal, then each mode melts it into something you might not recognize, all driven by the music:

- **Bars**: glitch art. The picture tears into slices with the spectrum, pixelates, splits into RGB and flashes inverted on kicks.
- **Ring**: a spinning kaleidoscope mandala.
- **Scope**: the waveform smears it sideways, and its colors turn into a heat map.
- **Lissajous**: it twists into a whirlpool around the center and gets solarized.
- **Tunnel**: it's wrapped around the inside of an endless tube you fly through.
- **Liquid**: the picture itself turns liquid and keeps its own colors. It bulges with the bass, twists into a whirlpool, folds into a new mirror symmetry every few seconds, ripples outward with each frequency, splits into RGB on kicks and leaves glowing echo trails.
- **Dark Fantasy**: the picture becomes the scene, graded to cold moonlight and breathing with the bass, with lightning, fog and bats over it. Its bright spots flicker with the music.
- **Pop**: the picture gets cel-shaded into a few bold colors with black ink outlines. Starbursts bulge the picture out around them and rings send ripples through it.
- **Dark Forest**: your picture is what's printed on the pages pinned to the trees.
- **Dystopia**: the picture is stretched to fill the giant screens hanging on the towers.
- **Rage**: the picture plays on the giant LED wall behind the stage, in red.
- **Hijack**: the picture is posterized into neon halftone behind everything, torn-out pieces of it get slapped on as stickers, and it pops up in "screenshot" windows.
- **Night Drive**: the picture goes up on billboards you drive past, and shows as album art on the dashboard screen.
- **Oscilloscope**: the picture's outlines are traced and drawn by the laser beam, like oscilloscope music. The music shakes the lines, pumps the size and spins it all in 3D. Press Tab for the normal sweep.

PNG, JPG, BMP, GIF and TGA all work. Press **I** again (or **Delete**) to remove it and get the plain effects back.

### Settings

Press **S** to open the settings panel for the current mode. Drag a slider or click a switch and the change shows up right away. Your settings are saved and come back next time. Each section has a **Reset** button.

- **Reactivity** (all modes): sensitivity, smoothing, kick strength, glow, particles.
- **Bars**: bar count, bar width, height, peak caps, reflection.
- **Ring**: ring size, bar length, bar count, spin speed, inner waveform.
- **Scope**: wave height, line thickness, spectrum below and its height.
- **Lissajous**: size, line thickness, number of copies, spin speed, trail length.
- **Tunnel**: fly speed, spin speed, ring size, trail length, kaleidoscope.
- **Liquid**: flow speed, turbulence, zoom, gloss, kaleidoscope.
- **Oscilloscope**: screen size, beam thickness, brightness, persistence, grid.
- **Dark Fantasy**: fly speed, render quality (lower it for more FPS), window glow, castle height, number of towers, moon size, lightning, bats, fog, cloud speed.
- **Night Drive**: speed, render quality, rain, traffic, streetlights, city skyline, dashboard on/off.
- **Dark Forest**: walk speed, render quality, flashlight brightness, fog, static, tree density, pages on/off.
- **Hijack**: glitch amount, popups, stickers, halftone dot size, big words on/off, terminal on/off, spectrum blocks on/off.
- **Rage**: render quality, strobes, camera shake, number of light beams, crowd size, glowing faces on/off, bell sparkles, grain, drop words on/off.
- **Dystopia**: fly speed, render quality, color scheme, window lights, smog, flashes, giant screens on/off, surveillance HUD on/off.
- **Pop**: starbursts, picture warp, rings, confetti, spectrum arch on/off and its size, outline thickness, cloud speed.

The panel is never part of exported videos, but your settings are.

### Filters

Press **G** to put a filter over any mode (Shift+G goes back). Filters work with pictures too and are included in exported videos:

**VHS** (wobbly tape, torn lines on kicks, bleeding color), **CRT** (curved screen, phosphor mask, scanlines), **Film** (warm grade, grain, flicker, dust), **Noir** (punchy black and white), **Thermal** (heat camera), **Neon edges** (only the outlines, glowing), **Pixel** (chunky pixels that grow on kicks), **Chromatic** (red and blue split apart on the beat), **Kaleidoscope**, **Mirror**, **Dream** (soft glow, pastel), **Duotone** (purple to pink/cyan) and **Halftone** (printed dots that swell with the beat).

### Export a video

Press **E** to save the current mode as an MP4 with the song's audio, ready to upload. It renders every frame at full quality, 60 fps, no matter how fast your computer is, and uses the canvas shape you picked with **A**:

| Canvas | Video size |
| --- | --- |
| Fit window or 16:9 | 1920 x 1080 |
| 9:16 | 1080 x 1920 |
| 1:1 | 1080 x 1080 |
| 4:5 | 1080 x 1350 |

A preview and progress bar show while it works, and Esc cancels. With a picture loaded, the video starts with it clear and lets it melt. Export uses the video encoder built into Windows; if that isn't available (for example on Windows "N" editions), it uses ffmpeg instead when it's installed.

## Download

Grab the latest `Bxsim.exe` from the [Releases](../../releases) page. It runs on Windows 10 and 11 and needs no installer or extra files: just download it and run it.

Windows may show a SmartScreen warning the first time, because the app isn't code-signed. Click **More info → Run anyway**.

## Using it

Get a song in any of these ways:

- Drop an audio file onto the window (you can drop an image along with it).
- Drag it onto `Bxsim.exe` in File Explorer.
- Press **O** (or click the start screen) to browse for one.

Supported formats are MP3, WAV, OGG and FLAC. When a song ends, Bxsim moves on to the next one in the same folder.

| Key | Action |
| --- | --- |
| Space | Play / pause |
| ← / → | Seek 5 s (hold Shift for 15 s) |
| ↑ / ↓ or mouse wheel | Volume |
| M (Shift+M for back) | Next mode |
| 1–9, 0, -, =, [, ] | Jump to a mode (0 is Night Drive, - is Dark Forest, = is Hijack, [ is Rage, ] is Dystopia) |
| Tab | Oscilloscope: switch XY (or image laser) / sweep |
| I | Add a picture, or remove the current one |
| Delete | Remove the picture |
| N / P | Next / previous song in the folder |
| L | Loop the current song |
| O | Open a song |
| S | Settings panel |
| G (Shift+G for back) | Next filter |
| A (Shift+A for back) | Canvas shape: fit window, 16:9, 9:16, 1:1, 4:5 |
| E | Export a video of the whole song |
| F11, F or double-click | Fullscreen on/off (Esc also leaves) |
| F3 | Show FPS |
| Esc | Quit |

Click or drag on the progress bar to seek. The controls fade out while you watch and come back when you move the mouse.

Bxsim opens as a maximized window. For content creation, press **A** to switch the canvas shape: 16:9 for YouTube, 9:16 for TikTok, Shorts and Reels, 1:1 square, or 4:5 for Instagram posts. The visuals are framed in that shape inside the window, ready to screen-record.

## Code tour

- `src/Spectrum.*` — the FFT, log-spaced frequency bands, smoothing, peak caps and beat detection.
- `src/Renderer.*` — all drawing: the fourteen modes, the filters, their GLSL shaders, the reacting picture, particles, bloom and on-screen UI.
- `src/Platform.*` — Windows bits: file dialogs, drag-and-drop, window maximizing and system fonts.
- `src/VideoWriter.*` — MP4 export (H.264 + AAC) through Windows Media Foundation, with an ffmpeg fallback.
- `src/Settings.*` and `src/SettingsPanel.*` — every adjustable setting, saving them, and the S panel.
- `src/main.cpp` — window, playback and input.
