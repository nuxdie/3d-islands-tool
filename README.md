# Landscape Water Islands

A seeded floating-island landscape with metre-based terrain, rivers, lakes, and GPU shallow-water dynamics. The default main landmass is **10,000 metres wide**; smaller, elevated islands have independent water layers and can spill onto lower islands.

## Representation and scale

- **Terrain:** closed marching-tetrahedra shells retain rocky undersides and volumetric cavities. Their upper surfaces are reshaped into kilometre-scale watersheds, with meandering channels and lake basins. A detailed watershed cap uses the same grid and triangulation as the water, so coarse rock triangles cannot cover narrow riverbeds.
- **Geometry LOD:** three prebuilt levels per volumetric shell, selected by camera distance. The upper watershed cap stays aligned with the simulation grid. This is whole-island geometry LOD, not a camera-following simulation clipmap.
- **Simulation tiles:** a 512 × 512 tile for the main island and 256 × 256 tiles for satellites, packed into a GPU atlas. Cell widths are measured in metres, independently for each island. The main tile has roughly 21 m cells including its dry boundary padding. This implementation targets regional river/lake views, not sub-metre shoreline interaction.
- **Scale cues:** metre-sized terrain texture detail, geological strata, atmospheric distance haze, camera distances in kilometres, and panning across the landscape. Rain streaks are only 0.6 m long and appear near the camera at low altitude; there are no landscape-sized droplet meshes.

## GPU water

The primary water representation is a **depth-averaged shallow-water solver**, not a particle fluid:

1. Two GPU passes compute shared finite-volume fluxes for the horizontal axes using Rusanov fluxes and hydrostatic reconstruction.
2. A third pass updates water depth and horizontal momentum. Bed-pressure corrections preserve a lake at rest over uneven terrain. A shared donor limiter keeps water depths nonnegative, and implicit Manning friction damps flow.
3. The solver advances at a fixed 60 Hz independently of rendering. A 25 m/s horizontal velocity bound and conservative flux limiting bound extreme flows. No water-depth cap discards accumulated lake volume.
4. Waterfall outlets transfer discharge between island tiles, with cell-area conversion so different resolutions conserve cubic metres of water. Water going into the open sky leaves the domain. Transfer is immediate; travel time through the waterfall is visual rather than a separate stored-water state.
5. Connected water surfaces and gravity-curved waterfall sheets are drawn from static GPU meshes. Surface elevation, visibility, flow shading, and wetness come from simulation textures. There is no per-frame particle neighbourhood search, fluid isosurface extraction, terrain raycasting, or mesh upload.

Channels and basins begin with modest water depths, so the initial scene already contains rivers and lakes. Local spring/runoff sources provide continuing discharge. Rainfall is 7.2 mm/hour (0.000002 m/s), rather than large batches of visible water particles. Source rates, cell areas, time steps, and depth all use SI units.

Each island has its own upper-surface water layer. Overlapping islands remain separate and exchange flow only through explicit outlets. Volumetric cavities are retained in the rock shell, but underground water flow within one island is not represented by the upper-surface solver.

## Build

You need CMake 3.20+, a C++20 compiler, Git, and desktop OpenGL development packages. The first configure downloads Raylib 5.5. The default landscape application uses OpenGL 3.3 shaders and does **not** require PhysX, CUDA, or `nvcc`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
./build/noise_islands
```

The previous PhysX particle implementation is retained as an optional experiment/test target. Enable it with `-DISLANDS_BUILD_PARTICLE_EXPERIMENT=ON`; it requires Linux x86_64, NVIDIA drivers, and downloads the matching PhysX GPU package. It is not used by the landscape renderer.

## Controls

| Input | Action |
| --- | --- |
| Left mouse drag | Orbit |
| Right mouse drag | Pan across the landscape |
| Mouse wheel | Distance-scaled zoom |
| `Space` | Generate a new seed |
| `W` | Toggle wireframe |
| `R` | Toggle automatic rotation |
| `Esc` | Quit |

The sidebar controls island count, scale, relief, roughness, and the volumetric shell's cave parameters. Slider changes rebuild when the mouse is released, so expensive terrain preparation does not interrupt every drag event. The 3D camera viewport occupies the area to the left of the sidebar.

## Verification and benchmarking

```bash
ctest --test-dir build --output-on-failure
```

The CPU suite checks closed terrain topology and geometric queries. The GPU suite runs the actual shaders to check:

- hydrostatic equilibrium over uneven ground;
- momentum transport, wetting, and pool merging;
- water-volume conservation in closed basins;
- conservative waterfall transfer between unequal-resolution layers;
- source rates in cubic metres per second;
- downward camera-local rain motion after hours of scene time;
- connected surface rendering;
- the generated main landmass's actual 10 km width and metre-scale lake depths.

GPU tests require a display/OpenGL context and skip if no display is available.

```bash
./build/noise_islands --seed 2851188872 --benchmark 300 --warmup-seconds 10 --screenshot screenshots/landscape-water.png --hidden
```

Benchmark mode renders the full scene to a 1280 × 760 offscreen target without VSync or the normal 60 FPS cap. It excludes terrain preparation and 30 warm-up frames, and waits for GPU completion before reporting time. It advances 1/60 second of water simulation per rendered frame. The target is not multisampled. Optional `--yaw` and `--pitch` arguments set the camera angles in radians. Shaders are embedded in the executable, so its working directory does not affect shader loading.
