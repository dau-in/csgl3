> [!WARNING]
> Heavily incomplete, not usable for normal play

## Requirements

* OpenGL 2.1 or later on SM3 capable hardware*
* Any Steam version of the game on either Windows or Linux

<sub>*OpenGL 3.x capable hardware required for a sane experience</sub>

## Missing features

* Most of the EFX API is not implemented
* Most beam types are not implemented
* Most rendering related cvars do not work with the renderer (e.g. gl_wireframe, r_fullbright)
* Studio model texture color remaps are not implemented
* Spectator subviews do not draw properly
* Tiling textures are not implemented

## Behavioral differences

The renderer tries to remain faithful to the engine's renderer, but there are some intentional and incidental differences:

* Dlights are applied per-pixel and do not scale with the lightmap
* Dlights are limited to 4 visible at a time
* Studio model lightmap sampling is interpolated
* Lightstyles are interpolated
* Water waves have no varying height
* Fog behaviour on translucent entities is different
* Underwater fog uses an exponential-squared falloff rather than linear
* Chrome textures on models are not constrained to 64x64 and may appear different
* Fullbright texture flag on studio models is supported
* Skybox textures are no longer limited to 256x256, but all faces must have the same size
* NVGs will not spawn dlights

## Installation

* Download and install [Client-side loader](https://github.com/mikkokko/csldr)
* Download the latest release for your platform from the [releases page](https://github.com/mikkokko/csgl3/releases/latest)
* Move `render.dll` / `render.so` to the `cl_dlls` folder (where `client.dll` / `client.so` resides)
* Launch the game. The cvar gl3_enable should be available

## Timedemos on a low-end system

Specs: AMD A6-3620, Radeon HD 6530D\
Settings: 1280x720, fullscreen, `-nofbo`\
Game build: 10210

| Level | Engine renderer | GL3 renderer | Speedup |
|-|-:|-:|-:|
| de_dust2 | 170 | 295 | 1.7x |
| surf_cyberwave | 29 | 129 | 4.4x |
| chk_section | 29 | 126 | 4.3x |
| de_safehouse_csgo | 51 | 131 | 2.6x |

More powerful cards will show greater gains with the renderer, though you likely won't have issues running the game over 100 fps with these.
