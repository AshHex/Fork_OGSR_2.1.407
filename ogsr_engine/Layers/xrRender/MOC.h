// OGSR MOC integration stage 1: immutable per-frame CPU occlusion snapshot.
#pragma once
#include <memory>
#include <atomic>

// Intel's Apache-2.0 implementation is not part of the uploaded source trees.
// Enable the actual MOC backend only when its header and .cpp implementation
// are installed. Without them the original HOM remains the working fallback.
#if defined(__has_include)
# if __has_include("MaskedOcclusionCulling.h")
#  define OGSR_USE_INTEL_MOC 1
# endif
#endif

class CFrustum;
class CGameFont;

class COGSRMOC
{
private:
    struct Face
    {
        Fbox box;
        Fplane plane;
        Fvector center;
        u32 flags;
    };
    struct Cluster
    {
        Fbox box;
        Fvector center;
        xr_vector<u32> indices;
        Cluster() { box.invalidate(); center.set(0.f,0.f,0.f); }
    };
    struct Snapshot;
    xr_vector<Face> m_faces;
    xr_vector<Fvector> m_vertices;
    xr_vector<Fvector> m_autoVertices;
    xr_vector<Cluster> m_autoClusters;
    u32 m_autoInputTriangles = 0;
    bool m_autoReady = false;
    bool LoadAuto();
    bool ReadCache(u64 hash);
    void WriteCache(u64 hash);
    u64 ComputeSourceHash() const;
    // C++20: std::atomic<shared_ptr> (non-deprecated); C++17: free atomic overloads.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const Snapshot>> m_snapshot;
#else
    std::shared_ptr<const Snapshot> m_snapshot;
#endif
    std::shared_ptr<const Snapshot> LoadSnapshot() const;
    std::shared_ptr<const Snapshot> ExchangeSnapshot(std::shared_ptr<const Snapshot> next);
    // Writer-only spare buffer. Reuse only when no parallel reader owns it.
    std::shared_ptr<Snapshot> m_spare;
    bool m_sourceLoaded = false;
    mutable std::atomic<u32> m_tests{0};
    mutable std::atomic<u32> m_rejected{0};

    BOOL TestRect(float xmin, float ymin, float xmax, float ymax, float clipW, const std::shared_ptr<const Snapshot>& snap) const;

public:
    COGSRMOC() = default;
    ~COGSRMOC();
    COGSRMOC(const COGSRMOC&) = delete;
    COGSRMOC& operator=(const COGSRMOC&) = delete;
    bool Load();
    void Unload();
    void Build(CFrustum& frustum);
    BOOL Visible(const Fbox& box) const;
    BOOL VisiblePadded(const Fbox& box, float padPixels) const;
    BOOL Visible(const sPoly& polygon) const;
    bool Active() const;
    void Stats(CGameFont& font) const;
    void DebugDraw() const;
};

extern int ps_r__moc_enable;
extern int ps_r__moc_draw;

// Capture only main level.geom, never the fast/alternate level.geomX.
// Buffers are released after AutoHOM has been built or cache loaded.
void OGSRMOC_CaptureVertexBuffer(u32 id, const BYTE* bytes, u32 size, u32 stride, u16 positionOffset, u8 positionType);
void OGSRMOC_CaptureIndexBuffer(u32 id, const u16* indices, u32 count);
void OGSRMOC_ClearGeometryCapture();
extern int ps_r__moc_source;

extern int ps_r__moc_cull_shadows;
extern int ps_r__moc_cull_lights;
extern int ps_r__moc_shadow_guard;
