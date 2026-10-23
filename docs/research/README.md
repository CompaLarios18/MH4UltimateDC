# Research notes

One Markdown file per topic (file formats, structs, subsystems). Cite addresses with the region/version they apply to.

| Note | What it covers |
|---|---|
| [first-look-v130.md](first-look-v130.md) | Layout of the USA v1.3.0 code, crt0, first engine/SDK observations |
| [mt-classes.md](mt-classes.md) | MT Framework class registry (992 classes), resource types |
| [system-services.md](system-services.md) | System calls and 3DS services the game uses; frame pacing (GSP, VBlank, present, frame limiter), input, local play, files |
| [community-enhancements.md](community-enhancements.md) | What the community already has: 60 fps cheats, widescreen, HUD, multiplayer through Azahar rooms, texture packs, format docs |
| [widescreen.md](widescreen.md) | Aspect ratio, projection, render targets, culling and HUD anchoring for a wider top screen; hook plan |
| [second-screen.md](second-screen.md) | What draws the bottom screen, touch → panel actions, layout views; staged plan for a PC second-screen adaptation |
| [screen-filters.md](screen-filters.md) | The engine's post-processing passes (bloom, depth of field, motion and radial blur, haze, colour correction): classes, where MH4U uses them, how the port turns them off |
| [metal.md](metal.md) | Metal on macOS: what MoltenVK costs Azahar's Vulkan renderer, what a native Metal backend would take, and the measurements that decide between them (MoltenVK now) |
| [rhi-spike.md](rhi-spike.md) | The evidence for D4 (RHI and backends): what the RX 6700 XT, the M2 (stand-ins until the Mac probe run) and lavapipe offer, plume against the renderer's needs (read, run on lavapipe, run under the validation layer), the three options costed, and a recommendation; the experiment's files are in [rhi-spike-evidence/](rhi-spike-evidence/README.md) |
| [android-fiber-jni.md](android-fiber-jni.md) | Android save-data crash: ART treats JNI calls from the recompiled-code fiber stack as a stack overflow; caching the build flavor and user directory keeps the fiber out of Java |
