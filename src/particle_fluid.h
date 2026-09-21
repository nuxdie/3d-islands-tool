#pragma once

#include "raylib.h"
#include <memory>
#include <span>
#include <vector>

// PhysX owns the CUDA particle buffers. Rendering consumes a device-to-device
// copy through CUDA/OpenGL interop; readPositions is for regression tests only.
class ParticleFluid {
public:
    ParticleFluid(std::span<const Vector3> triangles, std::span<const Vector4> particles,
                  float spacing = 0.14F, bool recycle = true);
    ~ParticleFluid();
    ParticleFluid(const ParticleFluid&) = delete;
    ParticleFluid& operator=(const ParticleFluid&) = delete;
    void step(float dt);
    void draw(Matrix view, Matrix projection, int width, int height);
    std::vector<Vector4> readPositions() const;
    unsigned int count() const;
    unsigned int surfaceTriangles() const;
    struct Impl;
private:
    std::unique_ptr<Impl> impl;
};
