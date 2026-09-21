# Volumetric Noise Islands

An interactive C++ visualization that turns a seeded 3D density field into floating, cave-filled islands. Each island begins as a random organic cluster of differently sized metaballs rather than a fixed ellipsoid. The seed controls its lobe count, silhouette, placement, elevation, proportions, rock surface, and cave field. Seeded cloud formations hover above the islands and continuously emit animated rain. Droplets stop on the extracted top surface, darken the wet terrain, collect in depressions to form spreading lakes, overflow into downhill streams and rivers, then spill from island edges as waterfalls. Unlike a heightmap, the volume supports overhangs, undersides, tunnels, and enclosed voids. A marching-tetrahedra pass extracts the visible isosurface from fractal rock and cave noise.

The surface uses an 88 × 60 × 88 sampling grid, consistent outward triangle winding, and a smooth boundary limit to keep large islands closed. Rivers are sampled against the extracted triangles and rendered as terrain-following water ribbons. Waterfalls follow the island lip before accelerating under gravity, collide with lower terrain, and feed water into the receiving surface. Hydrology still uses a single top-surface layer per grid column; it does not simulate separate underground streams beneath overhangs.

## GPU water and connected lakes

- Water depth, rainfall, downhill flux, and wetness run in floating-point GPU textures at a fixed 60 Hz, using OpenGL 3.3 fragment passes. Water exchanges with all four neighboring cells according to surface elevation, so adjacent pools spread and settle to a shared level. The former per-cell storage cap is removed.
- Lakes use a connected, GPU-displaced surface with terrain-clipped shorelines instead of individual cylinders. Shallow runoff darkens the terrain; accumulated flows produce animated river ribbons.
- River banks, waterfall paths, and receiving-island connections are baked once when the terrain changes. River widths/visibility, water ripples, rain particles, and terrain wet shading are computed by shaders. The running simulation has no CPU terrain raycasts, per-vertex color rewrites, mesh uploads, or GPU readbacks.
- Rain uses fixed emission heights and lifetimes so cloud bobbing cannot move live drops upward. Drops disappear at terrain/water impact and respawn at new positions instead of repeatedly jumping up the same columns.
- Terrain generation and water-path baking still run on the CPU during startup and generator changes. Clouds reuse an uploaded sphere mesh.

## Build

The first configure downloads Raylib 5.5. You need CMake 3.20+, a C++20 compiler, Git, and the usual desktop OpenGL development packages.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/noise_islands
```

Run the regression checks after building:

```bash
ctest --test-dir build --output-on-failure
```

The geometry tests are headless. The GPU test needs an OpenGL display/context (and skips when no display is available). It runs the actual shaders and verifies water conservation, pool merging over a low ridge, level equalization, continuity of the rendered lake surface, and downward rain motion at scene times up to six hours.

For a reproducible performance check and screenshot:

```bash
./build/noise_islands --seed 3786946813 --benchmark 600 --warmup-seconds 180 --screenshot screenshots/gpu-water.png --hidden
```

The benchmark renders to a 1280 × 760 offscreen target without VSync or the normal 60 FPS cap. It excludes setup and 30 warm-up frames, waits for GPU completion before reporting timing, and renders one water simulation step per frame. `--warmup-seconds` fills the basins before measuring. Offscreen rendering also makes hidden-window runs meaningful; the benchmark target is not multisampled. Optional `--yaw` and `--pitch` arguments set the camera angle in radians. Shaders are embedded in the executable, so launching it does not depend on the working directory.

## Controls

| Input | Action |
| --- | --- |
| Left mouse drag | Orbit the camera |
| Mouse wheel | Zoom |
| `Space` | Generate a new random island |
| `W` | Toggle solid/wireframe mode |
| `R` | Toggle automatic rotation |
| `Esc` | Quit |

The sidebar controls island count, overall and vertical scale, surface roughness, cave size, and cave strength. Drag a slider or hover it and use the mouse wheel; the mesh updates immediately. Use **New Random Seed** to create a different configuration with the selected parameters.
