# Volumetric Noise Islands

An interactive C++ visualization that turns a seeded 3D density field into floating, cave-filled islands. Each island begins as a random organic cluster of differently sized metaballs rather than a fixed ellipsoid. The seed controls its lobe count, silhouette, placement, elevation, proportions, rock surface, and cave field. Seeded clouds emit animated rain, while NVIDIA PhysX GPU fluid particles flow over the islands, collect in depressions, and fall onto lower surfaces. Unlike a heightmap, the volume supports overhangs, undersides, tunnels, and enclosed voids. A marching-tetrahedra pass extracts the visible isosurface from fractal rock and cave noise.

The terrain uses an 88 × 60 × 88 sampling grid, consistent outward triangle winding, and a smooth boundary limit to keep large islands closed. PhysX cooks the actual island triangles for GPU collision detection, including lower surfaces and overhangs.

## NVIDIA GPU fluid simulation

- **Solver:** PhysX 5.4.2's CUDA position-based fluid solver, with gravity, particle interactions, viscosity, surface tension, and full triangle-mesh collisions. Each 60 Hz update uses two physics substeps. There are no fixed river paths or grid-cell ribbon connections.
- **Surface:** PhysX's GPU smoothing, anisotropy, and sparse-grid isosurface extractor reconstruct and smooth a triangle mesh from the fluid. CUDA writes vertices, normals, and indices directly into OpenGL-shared buffers. The renderer adds thickness-based absorption, refraction, reflections, and scene-depth occlusion.
- **Water supply:** Runoff source patches beneath clouds initialize a bounded particle budget (up to 32,768 particles). Water leaving the bottom of the volume recycles to its source on the GPU; settled pool particles remain in place. This maintains a bounded simulation rather than continually allocating new fluid. Sources are a visual approximation of accumulated rainfall, separate from the decorative rain streaks.
- **Rain and wetness:** GPU rain uses fixed emission heights and lifetimes, so cloud bobbing cannot move live drops upward. Terrain clips drops, and rendered water occludes them. A small GPU texture tracks rainfall wetness for terrain shading; it no longer drives water dynamics.
- **CPU work:** Terrain generation, collision-mesh cooking, and initial particle setup occur during startup/rebuilds. Runtime particle state and surface geometry stay on the GPU. PhysX synchronizes mesh counts and simulation completion with the CPU; full particle readbacks are used only in tests.

## Build

This build targets **Linux x86_64 with an NVIDIA GPU and working NVIDIA OpenGL/CUDA drivers**. You need CMake 3.20+, a C++20 compiler, Git, and desktop OpenGL development packages. A CUDA toolkit or `nvcc` installation is not required: the application uses driver APIs and NVIDIA's matching prebuilt PhysX GPU solver.

The first configure downloads Raylib 5.5, PhysX 5.4.2, and the approximately 237 MiB compressed NVIDIA GPU runtime package into `build/_deps`. The GPU archive is SHA-256 verified. The first build compiles the PhysX host libraries and takes longer than subsequent builds. The executable uses the runtime under `build/_deps/physx_gpu-src`; retain that dependency directory when running it. Upstream SDK/runtime licenses are included with the downloaded packages.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6
./build/noise_islands
```

Run the regression checks after building:

```bash
ctest --test-dir build --output-on-failure
```

The geometry tests are headless. Rain and PhysX tests need a display/OpenGL context; PhysX additionally needs an NVIDIA GPU. GPU tests skip when no display is available. Tests cover mesh closure, terrain queries, downward rain motion at times up to six hours, fluid settling on triangles, pool merging inside a basin, separate stacked collision surfaces, particle conservation, recycling, GPU surface extraction/rendering, resizing, and repeated scene creation/teardown.

For a reproducible performance check and screenshot:

```bash
./build/noise_islands --seed 2851188872 --benchmark 300 --warmup-seconds 10 --screenshot screenshots/physx-water.png --hidden
```

The benchmark renders to a 1280 × 760 offscreen target without VSync or the normal 60 FPS cap. It excludes setup and 30 warm-up frames, waits for GPU completion before reporting timing, and simulates 1/60 second per frame. `--warmup-seconds` lets the fluid settle before measuring. Offscreen rendering makes hidden-window runs meaningful; the target is not multisampled. Optional `--yaw` and `--pitch` arguments set the camera angle in radians. Shaders are embedded in the executable.

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
