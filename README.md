# Volumetric Noise Islands

An interactive C++ visualization that turns a seeded 3D density field into floating, cave-filled islands. Each island begins as a random organic cluster of differently sized metaballs rather than a fixed ellipsoid. The seed controls its lobe count, silhouette, placement, elevation, proportions, rock surface, and cave field. Seeded cloud formations hover above the islands and continuously emit animated rain. Droplets stop on the extracted top surface, darken the wet terrain, collect in depressions to form spreading lakes, overflow into downhill streams and rivers, then spill from island edges as waterfalls. Unlike a heightmap, the volume supports overhangs, undersides, tunnels, and enclosed voids. A marching-tetrahedra pass extracts the visible isosurface from fractal rock and cave noise.

The surface uses an 88 × 60 × 88 sampling grid, consistent outward triangle winding, and a smooth boundary limit to keep large islands closed. Rivers are sampled against the extracted triangles and rendered as terrain-following water ribbons. Waterfalls follow the island lip before accelerating under gravity, collide with lower terrain, and feed water into the receiving surface. Hydrology still uses a single top-surface layer per grid column; it does not simulate separate underground streams beneath overhangs.

## Build

The first configure downloads Raylib 5.5. You need CMake 3.20+, a C++20 compiler, Git, and the usual desktop OpenGL development packages.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/noise_islands
```

Run the headless geometry and water-path regression checks after building:

```bash
ctest --test-dir build --output-on-failure
```

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
