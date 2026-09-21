# Volumetric Noise Islands

An interactive C++ visualization that turns a seeded 3D density field into floating, cave-filled islands. Each seed controls the island count, placement, elevation, proportions, rock surface, and cave field. Unlike a heightmap, the volume supports overhangs, undersides, tunnels, and enclosed voids. A marching-tetrahedra pass extracts the visible isosurface from fractal rock and cave noise.

## Build

The first configure downloads Raylib 5.5. You need CMake 3.20+, a C++20 compiler, Git, and the usual desktop OpenGL development packages.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/noise_islands
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
