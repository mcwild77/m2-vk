Why does this exist?

This mainly exists so we can run MAME 3D games inside Age of Joy, a VR/MR virtual arcade. Age of Joy is a Meta Quest 2/3 native app that gives you an empty arcade and lets you fill it up with user-created cabinets. Emulation is handled through Retroarch cores.

While the Quest 2/3 are able to handle older games, the strain the software rendering of modern MAME puts on the CPU makes games choppy or unplayable. The solution is to make a custom core where we rewrite MAME's renderer to use Vulkan. This is actually not enough to hit frame rate, but it comes close. The other trick is doing a bunch of optimizations and omissions, like deactivating certain boards and threading the audio, to lessen the strain on a single-threaded mobile CPU core.


This is based on MAME. It is not as good as MAME and never will be. It will have the same issues as MAME (if not more). It exists for QOL additions and playability on niche hardware, not for preservation or accuracy. It's also going to be behind mainline MAME releases, and even though I'll try to stay on top of rolling in the latest fixes from the main MAME branch, if I get hit by a bus it's probably not gonna to get updated. I'm one foot in the grave anyway.

HEY, I SEE EM-DASHES IN THE SOURCE CODE! DID YOU USE AI?
Yes.

DOES THIS DO ANYTHING COOL?
Aside from rendering in Vulkan, we can easily boost native rendering res, apply some nice smoothing fx, add filtering to Namco System22, and add shading to Model 1 (yuck lol). ElSemi had all of this sorted over 15 years ago with his Model 2 Emulator for Windows so this is in no way special.

For QOL purposes there is steering wheel curve customizability.  I've always found racing games a drag to play on gamepads via emulation as it's mapping full wheel rotation to 2cm of analog. You've got a curve for more sensitivity at lower inputs, the ability to clamp input so you're not twisting the wheel to insane angles when you hold the stick all the way to the right. 

CORE OPTIONS

Core Options only show up when they apply to the hardware you've loaded, so you won't see the System 22 stuff on a Model 2 game, and you won't see steering options unless the cabinet has a wheel, etc. A few need a content reload to take effect (noted below); the rest apply live. All settings default to original/authentic board feature set (resolution, filtering) and optimizations targeted at CPU-starved devices (Quest).

RENDERING

3D Renderer — Hardware Vulkan or MAME's software rasterizer. Requires reload.
  • Vulkan (hardware) [default]
  • Software (MAME)

Internal Resolution — Render resolution. Vulkan only. Requires restart.
  • Native [default] — 496x384 on Model 2/Model 1, 496x480 on System 21, 640x480 on System 22
  • 496x480, 640x480, 1024x768, 1280x960, 1440x1080, 1600x1200, 1920x1440, 2560x1920, 2848x2136
  (This is native res, not supersampling)

Use Real Transparency — Off uses original screen-door transparency effect; On is real transparency. Model 2, Vulkan only.
  • Off [default]
  • On

Smooth 2D Backgrounds — Bilinear-filter the 2D background layer when Internal Resolution is above native. Softens the background tilemaps; the HUD text stays sharp. Vulkan only.
  • Off [default]
  • On

ENHANCEMENTS (not accurate — these are the "make it prettier/uglier" toggles)

Flat Shaded — Draws untextured geometry lit by hardware shading.
  • Off [default]
  • On

Unlit — Disable lighting for fullbright textures.
  • Off [default]
  • On

Smooth Shading (Model 2) — Take a crack at Gouraud shading Model 2 polys. Enhancement, not accurate.
  • Off [default]
  • On

Smooth Shading (Model 1) — Gouraud-shade Model 1's flat-shaded polygons. Enhancement, not accurate. Looks awful lol
  • Off [default]
  • On

SYSTEM 22 (Namco) ONLY

Texture Filtering — Add bilinear filtering to textures. System 22 only.
  • Off [default]
  • On

Fog — Enable or disable fog. System 22 only.
  • On [default]
  • Off

Flat Shaded — Draws untextured geometry lit by hardware shading. Greyscale, not too interesting.
  • Off [default]
  • On

2D Overlay (HUD) — Toggles 2D elements.
  • On [default]
  • Off

ON-SCREEN READOUTS

Polygon Count — Display polygons rendered per frame. Vulkan only. Who had the highest poly count of them all? Call Next Generation Magazine!
  • Off [default]
  • On

Frame Rate Counter — Show the emulated frame rate in the top-left corner. Green while holding the game's native rate, red when it drops. Vulkan only.
  • Off [default]
  • On

Steering Visualizer — On-screen display showing steering input vs. stick position.
  • Off [default]
  • On

STEERING (wheel cabinets only)

I've always found racing games a drag on gamepads — full wheel rotation mapped to 2cm of analog. These let you fix that.

Steering Response — Adjust curve on steering wheel input.
  • Linear, Slight [default], Medium, Strong, Very Strong

Steering Deadzone — Adjust percentage of steering wheel dead zone.
  • 0 (off), 2, 5 [default], 8, 10, 15, 20

Steering Range — Adjust how much of the full wheel input range is mapped to the stick. (Clamp this so you're not twisting to insane angles at full stick.)
  • 100 (full lock), 90, 80 [default], 70, 60

Steering Damping (Turn) — Adjust damping (in frames) when turning wheel. 0 is no effect.
  • 0, 2f, 4f, 8f [default], 16f

Steering Damping (Return) — Adjust damping (in frames) when controller is released and wheel snaps back to center. 0 is no effect. I think this makes most games feel better, since most physical arcade wheels can't instantly snap back to neutral.
  • 0, 2f, 4f, 8f [default], 16f

GAMEPAD (analog stick)

Analog Deadzone — Adjust gamepad deadzone. Increase if you see jittering.
  • 0 (off), 2, 5 [default], 10, 15

Analog Reach — Controls how much of the stick's analog movement is mapped to the game.
  • 100 (full deflection) [default], 95, 90, 85, 80, 75

PERFORMANCE / HARDWARE

Threaded Sound — Run the sound CPU on its own thread for speed on multi-core devices. Original Model 2 board sets only; needs a reload.
  • On [default]
  • Off

Fast Sound-Link Timing — Clock the sound board's serial link only when it can act, instead of a million times an emulated second. Large speed-up on CPU limited platforms; the bytes on the link are unchanged, but device timing shifts slightly, so a few games render a frame differently. Needs a reload. (Model 1 / Model 2 only.)
  • On [default]
  • Off

JVS HLE I/O Board (Time Crisis 2 / Crisis Zone) — Replace the light-gun cabinet's JVS I/O board CPU with a direct implementation of the JVS protocol it speaks, instead of interpreting the board's own processor. Big perf increase on weaker devices. Gun, pedal, and coin inputs are read the same way either way. Needs a reload. (System 23 only.)
  • On (default on Android) / Off (default on desktop)

Self-Paced Timing — The core paces itself to the game's native rate instead of trusting the frontend's frame limiter. Fixes frame-rate undershoot on devices whose frontend timer is imprecise (Android). Needs a reload.
  • On [default]
  • Off

Drive Board Emulation — Emulate the wheel cabinet's force-feedback drive board. No effect on a gamepad. Keep disabled to save CPU on weaker devices. Applies immediately. (Wheel cabinets only.)
  • On [default]
  • Off

Cabinet Billboard — Emulate the cabinet's LED marquee board. Never used, remains off by default to save CPU. Device timing shifts slightly between the two settings. Applies immediately.
  • Off [default]
  • On

SAVE STATES: Not supported. The core reports a savestate size of 0, so RetroArch greys the save/load slots out. The MAME team hasn't solved this yet, and I won't try.

WHERE ARE THE DOWNLOADS?
This will generate a clean core for Mac, Windows, and Android. You're welcome to 