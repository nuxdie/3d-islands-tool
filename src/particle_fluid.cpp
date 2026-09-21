#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
// Avoid Xlib's Font typedef colliding with raylib's Font.
extern "C" void* glXGetCurrentContext(void);
#include "particle_fluid.h"
#include "rlgl.h"
#include "raymath.h"
#include "PxPhysicsAPI.h"
#include "cudamanager/PxCudaContext.h"
#include "gpu/PxPhysicsGpu.h"
#include "PxIsosurfaceExtraction.h"
#include "PxAnisotropy.h"
#include "PxSmoothing.h"
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <string>

using namespace physx;

namespace {
struct GpuLoader : PxGpuLoadHook {
    const char* getPhysXGpuDllName() const override { return ISLANDS_PHYSX_GPU_LIBRARY; }
};
GpuLoader gpuLoader;
PxDefaultAllocator allocator;
struct ErrorHandler : PxErrorCallback {
    bool failed = false;
    void reportError(PxErrorCode::Enum code, const char* message, const char*, int) override {
        const bool error = code != PxErrorCode::eDEBUG_INFO && code != PxErrorCode::eDEBUG_WARNING;
        failed |= error;
        TraceLog(error ? LOG_ERROR : LOG_WARNING, "PHYSX: %s", message);
    }
};
void check(int result, const char* operation) {
    if (result != 0) throw std::runtime_error(std::string(operation) + " failed (CUDA error " + std::to_string(result) + ")");
}
template<class T> T symbol(void* library, const char* name) {
    auto function = reinterpret_cast<T>(dlsym(library, name));
    if (!function) throw std::runtime_error(std::string("Missing NVIDIA driver entry point: ") + name);
    return function;
}
// Small recycling kernel in portable PTX, JIT-compiled by the installed driver.
// PhysX supplies the fluid solver; this only returns out-of-world particles to
// their rain-runoff emitters. Settled water is never deleted/replaced.
constexpr const char* recyclePtx = R"PTX(
.version 6.0
.target sm_50
.address_size 64
.visible .entry recycle(
 .param .u64 positions, .param .u64 velocities, .param .u64 origins, .param .u32 count)
{
 .reg .pred p;
 .reg .b32 i, t, b, n;
 .reg .b64 pos, vel, src, offset, a, v, s;
 .reg .f32 x, y, z, w;
 ld.param.u64 pos, [positions];
 ld.param.u64 vel, [velocities];
 ld.param.u64 src, [origins];
 ld.param.u32 n, [count];
 mov.u32 i, %tid.x;
 mov.u32 t, %ctaid.x;
 mov.u32 b, %ntid.x;
 mad.lo.u32 i, t, b, i;
 setp.ge.u32 p, i, n;
 @p bra done;
 mul.wide.u32 offset, i, 16;
 add.u64 a, pos, offset;
 ld.global.f32 y, [a+4];
 setp.gt.f32 p, y, -18.0;
 @p bra done;
 add.u64 s, src, offset;
 add.u64 v, vel, offset;
 ld.global.v4.f32 {x,y,z,w}, [s];
 st.global.v4.f32 [a], {x,y,z,w};
 mov.f32 x, 0.0;
 mov.f32 y, -1.0;
 st.global.v4.f32 [v], {x,y,x,x};
done:
 ret;
}
)PTX";
}

struct ParticleFluid::Impl : PxParticleSystemCallback {
    ErrorHandler errors;
    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    PxCudaContextManager* cuda = nullptr;
    PxDefaultCpuDispatcher* dispatcher = nullptr;
    PxScene* scene = nullptr;
    PxMaterial* rock = nullptr;
    PxPBDMaterial* water = nullptr;
    PxTriangleMesh* mesh = nullptr;
    PxPBDParticleSystem* system = nullptr;
    PxParticleBuffer* particles = nullptr;
    PxAnisotropyGenerator* anisotropy = nullptr;
    PxSmoothedPositionGenerator* smoothing = nullptr;
    CUdeviceptr smoothed = 0;
    std::array<CUdeviceptr,3> axes{};
    bool simulated = false;
    CUdeviceptr origins = 0;
    CUmodule module = nullptr;
    CUfunction recycleKernel = nullptr;
    void* driver = nullptr;
    CUgraphicsResource interop = nullptr;
    int (*registerBuffer)(CUgraphicsResource*, unsigned int, unsigned int) = nullptr;
    int (*unregisterResource)(CUgraphicsResource) = nullptr;
    int (*mapResources)(unsigned int, CUgraphicsResource*, CUstream) = nullptr;
    int (*unmapResources)(unsigned int, CUgraphicsResource*, CUstream) = nullptr;
    int (*mappedPointer)(CUdeviceptr*, size_t*, CUgraphicsResource) = nullptr;
    GLuint buffer = 0, positionsTexture = 0, vao = 0;
    unsigned int size = 0;
    float spacing = 0.14F;
    bool recycle = true;
    struct Renderer;
    std::unique_ptr<Renderer> renderer;
    ~Impl();
    void onBegin(const PxGpuMirroredPointer<PxGpuParticleSystem>&, CUstream) override {}
    void onAdvance(const PxGpuMirroredPointer<PxGpuParticleSystem>&, CUstream) override {}
    void onPostSolve(const PxGpuMirroredPointer<PxGpuParticleSystem>& gpu, CUstream stream) override {
        anisotropy->generateAnisotropy(gpu.mDevicePtr,size,stream);
        smoothing->generateSmoothedPositions(gpu.mDevicePtr,size,stream);
    }
    void copyToOpenGL() {
        PxScopedCudaLock lock(*cuda);
        check(mapResources(1, &interop, nullptr), "Map OpenGL particle buffer");
        CUdeviceptr destination = 0;
        size_t bytes = 0;
        check(mappedPointer(&destination, &bytes, interop), "Get mapped particle buffer");
        check(cuda->getCudaContext()->memcpyDtoD(destination, simulated ? smoothed : reinterpret_cast<CUdeviceptr>(particles->getPositionInvMasses()), size * sizeof(PxVec4)), "Copy particles on GPU");
        check(unmapResources(1, &interop, nullptr), "Unmap OpenGL particle buffer");
    }
};

ParticleFluid::ParticleFluid(std::span<const Vector3> triangles, std::span<const Vector4> positions,
                             float spacing, bool recycle) : impl(std::make_unique<Impl>()) {
    auto& f = *impl;
    if (positions.empty()) throw std::runtime_error("Fluid needs at least one particle");
    f.size = static_cast<unsigned int>(positions.size());
    f.spacing = spacing;
    f.recycle = recycle;
    PxSetPhysXGpuLoadHook(&gpuLoader);
    f.foundation = PxCreateFoundation(PX_PHYSICS_VERSION, allocator, f.errors);
    if (!f.foundation) throw std::runtime_error("PhysX foundation initialization failed");
    f.physics = PxCreatePhysics(PX_PHYSICS_VERSION, *f.foundation, PxTolerancesScale());
    PxCudaContextManagerDesc cudaDesc;
    cudaDesc.graphicsDevice = glXGetCurrentContext();
    f.cuda = PxCreateCudaContextManager(*f.foundation, cudaDesc);
    if (!f.cuda || !f.cuda->contextIsValid()) throw std::runtime_error("PhysX GPU fluids require a working NVIDIA CUDA/OpenGL context");
    f.dispatcher = PxDefaultCpuDispatcherCreate(2);
    PxSceneDesc desc(f.physics->getTolerancesScale());
    desc.gravity = PxVec3(0, -9.81F, 0);
    desc.cpuDispatcher = f.dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.cudaContextManager = f.cuda;
    desc.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS | PxSceneFlag::eENABLE_PCM;
    desc.broadPhaseType = PxBroadPhaseType::eGPU;
    desc.solverType = PxSolverType::eTGS;
    f.scene = f.physics->createScene(desc);
    if (!f.scene) throw std::runtime_error("PhysX GPU scene creation failed");
    f.rock = f.physics->createMaterial(0.08F, 0.05F, 0.0F);
    if (!triangles.empty()) {
        std::vector<PxU32> indices(triangles.size());
        std::iota(indices.begin(), indices.end(), 0U);
        PxTriangleMeshDesc terrain;
        terrain.points.count = static_cast<PxU32>(triangles.size());
        terrain.points.stride = sizeof(Vector3);
        terrain.points.data = triangles.data();
        terrain.triangles.count = static_cast<PxU32>(triangles.size() / 3);
        terrain.triangles.stride = 3 * sizeof(PxU32);
        terrain.triangles.data = indices.data();
        PxCookingParams params(f.physics->getTolerancesScale());
        params.buildGPUData = true;
        params.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
        params.meshWeldTolerance = 0.00001F;
        f.mesh = PxCreateTriangleMesh(params, terrain, f.physics->getPhysicsInsertionCallback());
        if (!f.mesh) throw std::runtime_error("Could not cook island collision mesh for PhysX");
        auto* actor = f.physics->createRigidStatic(PxTransform(PxIdentity));
        PxRigidActorExt::createExclusiveShape(*actor, PxTriangleMeshGeometry(f.mesh), *f.rock);
        f.scene->addActor(*actor);
    }
    f.water = f.physics->createPBDMaterial(0.04F, 0.03F, 0.0F, 0.001F, 0.5F, 0.005F, 0.01F, 0.0F, 0.0F);
    f.water->setViscosity(0.01F);
    f.water->setSurfaceTension(0.00704F);
    f.water->setCohesion(0.0704F);
    f.water->setVorticityConfinement(2.0F);
    f.system = f.physics->createPBDParticleSystem(*f.cuda, 96);
    if (!f.system) throw std::runtime_error("PhysX PBD particle system creation failed");
    const float fluidRest = spacing * 0.5F;
    const float contact = fluidRest / 0.6F;
    f.system->setRestOffset(contact);
    f.system->setContactOffset(contact + 0.01F);
    f.system->setParticleContactOffset(contact);
    f.system->setSolidRestOffset(contact);
    f.system->setFluidRestOffset(fluidRest);
    f.system->setSolverIterationCounts(6, 1);
    f.system->setMaxVelocity(18.0F);
    f.system->setParticleFlag(PxParticleFlag::eENABLE_SPECULATIVE_CCD, true);
    f.scene->addActor(*f.system);
    const PxU32 phase = f.system->createPhase(f.water,
        PxParticlePhaseFlags(PxParticlePhaseFlag::eParticlePhaseFluid | PxParticlePhaseFlag::eParticlePhaseSelfCollide));
    f.particles = f.physics->createParticleBuffer(f.size, 0, f.cuda);
    if (!f.particles) throw std::runtime_error("PhysX particle buffer allocation failed");
    std::vector<PxVec4> initial(f.size), velocity(f.size, PxVec4(0.0F));
    std::vector<PxU32> phases(f.size, phase);
    const float mass = 1000.0F * spacing * spacing * spacing;
    for (size_t i = 0; i < positions.size(); ++i) initial[i] = PxVec4(positions[i].x, positions[i].y, positions[i].z, 1.0F / mass);
    {
        PxScopedCudaLock lock(*f.cuda);
        auto* context = f.cuda->getCudaContext();
        check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(f.particles->getPositionInvMasses()), initial.data(), initial.size() * sizeof(PxVec4)), "Initialize fluid positions");
        check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(f.particles->getVelocities()), velocity.data(), velocity.size() * sizeof(PxVec4)), "Initialize fluid velocities");
        check(context->memcpyHtoD(reinterpret_cast<CUdeviceptr>(f.particles->getPhases()), phases.data(), phases.size() * sizeof(PxU32)), "Initialize fluid phases");
        if (recycle) {
            check(context->memAlloc(&f.origins, initial.size() * sizeof(PxVec4)), "Allocate runoff emitters");
            check(context->memcpyHtoD(f.origins, initial.data(), initial.size() * sizeof(PxVec4)), "Initialize runoff emitters");
            check(context->moduleLoadDataEx(&f.module, recyclePtx, 0, nullptr, nullptr), "Load recycling kernel");
            check(context->moduleGetFunction(&f.recycleKernel, f.module, "recycle"), "Find recycling kernel");
        }
    }
    f.particles->setNbActiveParticles(f.size);
    f.system->addParticleBuffer(f.particles);
    {
        PxScopedCudaLock lock(*f.cuda);
        auto* context = f.cuda->getCudaContext();
        auto* gpu = PxGetPhysicsGpu();
        f.smoothing = gpu->createSmoothedPositionGenerator(f.cuda,f.size,0.5F);
        f.anisotropy = gpu->createAnisotropyGenerator(f.cuda,f.size,5.0F,1.0F,2.0F);
        if (!f.smoothing || !f.anisotropy) throw std::runtime_error("PhysX fluid smoothing initialization failed");
        check(context->memAlloc(&f.smoothed,f.size*sizeof(PxVec4)),"Allocate smoothed positions");
        for (auto& axis : f.axes) check(context->memAlloc(&axis,f.size*sizeof(PxVec4)),"Allocate fluid anisotropy");
        f.smoothing->setResultBufferDevice(reinterpret_cast<PxVec4*>(f.smoothed));
        f.anisotropy->setResultBufferDevice(reinterpret_cast<PxVec4*>(f.axes[0]),reinterpret_cast<PxVec4*>(f.axes[1]),reinterpret_cast<PxVec4*>(f.axes[2]));
    }
    f.system->setParticleSystemCallback(&f);
    f.driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!f.driver) throw std::runtime_error("NVIDIA CUDA driver library is unavailable");
    f.registerBuffer = symbol<decltype(f.registerBuffer)>(f.driver, "cuGraphicsGLRegisterBuffer");
    f.unregisterResource = symbol<decltype(f.unregisterResource)>(f.driver, "cuGraphicsUnregisterResource");
    f.mapResources = symbol<decltype(f.mapResources)>(f.driver, "cuGraphicsMapResources");
    f.unmapResources = symbol<decltype(f.unmapResources)>(f.driver, "cuGraphicsUnmapResources");
    f.mappedPointer = symbol<decltype(f.mappedPointer)>(f.driver, "cuGraphicsResourceGetMappedPointer_v2");
    glGenBuffers(1, &f.buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, f.buffer);
    glBufferData(GL_TEXTURE_BUFFER, f.size * sizeof(PxVec4), nullptr, GL_DYNAMIC_DRAW);
    glGenTextures(1, &f.positionsTexture);
    glBindTexture(GL_TEXTURE_BUFFER, f.positionsTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, f.buffer);
    glBindTexture(GL_TEXTURE_BUFFER, 0);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    glGenVertexArrays(1, &f.vao);
    {
        PxScopedCudaLock lock(*f.cuda);
        check(f.registerBuffer(&f.interop, f.buffer, 2), "Register CUDA/OpenGL fluid buffer");
    }
    f.copyToOpenGL();
    TraceLog(LOG_INFO, "PHYSX: %u GPU fluid particles, %.3f spacing, full triangle-mesh collisions", f.size, spacing);
}

void ParticleFluid::step(float dt) {
    auto& f = *impl;
    if (f.recycle) {
        PxScopedCudaLock lock(*f.cuda);
        CUdeviceptr positions = reinterpret_cast<CUdeviceptr>(f.particles->getPositionInvMasses());
        CUdeviceptr velocities = reinterpret_cast<CUdeviceptr>(f.particles->getVelocities());
        void* params[]{&positions, &velocities, &f.origins, &f.size};
        check(f.cuda->getCudaContext()->launchKernel(f.recycleKernel, (f.size + 127) / 128, 1, 1, 128, 1, 1,
              0, nullptr, params, nullptr, __FILE__, __LINE__), "Recycle escaped fluid");
        check(f.cuda->getCudaContext()->streamSynchronize(nullptr), "Finish fluid recycling");
        f.particles->raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
        f.particles->raiseFlags(PxParticleBufferFlag::eUPDATE_VELOCITY);
    }
    f.scene->simulate(dt);
    f.scene->fetchResults(true);
    f.scene->fetchResultsParticleSystem();
    f.simulated = true;
    if (f.errors.failed) throw std::runtime_error("PhysX reported a simulation error; see log");
}

unsigned int ParticleFluid::count() const { return impl->size; }
std::vector<Vector4> ParticleFluid::readPositions() const {
    auto& f = *impl;
    std::vector<Vector4> result(f.size);
    PxScopedCudaLock lock(*f.cuda);
    check(f.cuda->getCudaContext()->memcpyDtoH(result.data(), reinterpret_cast<CUdeviceptr>(f.particles->getPositionInvMasses()),
          result.size() * sizeof(Vector4)), "Read fluid test positions");
    return result;
}

// Screen-space fluid reconstruction is defined separately from the solver.
#include "fluid_renderer.h"

unsigned int ParticleFluid::surfaceTriangles() const {
    return impl->renderer ? impl->renderer->meshTriangles : 0;
}

ParticleFluid::Impl::~Impl() {
    renderer.reset();
    if (cuda) {
        PxScopedCudaLock lock(*cuda);
        if (interop) unregisterResource(interop);
        if (module) cuda->getCudaContext()->moduleUnload(module);
        if (origins) cuda->getCudaContext()->memFree(origins);
        if (smoothing) smoothing->release();
        if (anisotropy) anisotropy->release();
        if (smoothed) cuda->getCudaContext()->memFree(smoothed);
        for (auto axis : axes) if (axis) cuda->getCudaContext()->memFree(axis);
    }
    if (positionsTexture) glDeleteTextures(1, &positionsTexture);
    if (buffer) glDeleteBuffers(1, &buffer);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (particles) particles->release();
    if (scene) scene->release();
    if (mesh) mesh->release();
    if (rock) rock->release();
    if (water) water->release();
    if (dispatcher) dispatcher->release();
    if (physics) physics->release();
    if (cuda) cuda->release();
    if (foundation) foundation->release();
    if (driver) dlclose(driver);
}
ParticleFluid::~ParticleFluid() = default;
