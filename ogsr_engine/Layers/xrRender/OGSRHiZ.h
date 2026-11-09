#pragma once
#include <memory>
// Temporal hierarchical Z for OGSR DX11. GPU pyramid -> async readback -> immutable CPU snapshot.
// Intentionally independent of the Intel MOC implementation and TTAPI worker lifetime.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11DepthStencilView;
class CGameFont;

namespace OGSRHiZ
{
    // Both Hi-Z modes use this immutable per-build snapshot to rank/select
    // CPU MOC occluders. Only mode 2 independently culls scene objects.
    struct OccluderSelection
    {
        std::shared_ptr<const void> snapshot;
        Fmatrix full;
        Fvector camera;
        Fvector direction;
        unsigned frame = 0;
        bool enabled = false;
    };
    OccluderSelection BeginOccluderSelection(const Fmatrix& full, const Fvector& camera,
                                             const Fvector& direction, unsigned frame);
    bool OccludedForOccluder(const OccluderSelection& selection, const Fbox& box);
    void Capture(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11DepthStencilView* depth,
                 const Fmatrix& full, const Fvector& camera, const Fvector& direction, unsigned frame);
    // Poll the completed readback before TTAPI visibility tasks whenever possible.
    // Must be called only on the render thread with OGSR's immediate context.
    void PumpReadback(ID3D11DeviceContext* immediate);
    bool Occluded(const Fbox& box); // false means unknown OR visible; never reject on uncertain data
    bool SnapshotReady(); // fresh GPU depth is actually available for this frame
    bool OccludedDebug(const Fbox& box); // debug-only query, doesn't alter efficiency counters
    void Reset();
    void Stats(CGameFont& font);
}
extern int ps_r__hiz; // 0 off, 1 Hi-Z-assisted MOC selection, 2 selection + adaptive object culling
