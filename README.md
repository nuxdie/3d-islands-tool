# Fractal Noise Islands

An interactive C++ visualization that turns seeded fractal value noise into a 3D island. It includes domain-warped terrain, height-based coloring, water, an orbit camera, and wireframe mode.

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
