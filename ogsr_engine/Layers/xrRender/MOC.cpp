// OGSR-native adapter for Intel's MaskedOcclusionCulling (Apache-2.0).
// Render to a new context and atomically publish a read-only snapshot.
// Readers may run concurrently on OGSR's TTAPI worker threads.
#include "stdafx.h"
#include "MOC.h"
#include "../../xr_3da/GameFont.h"
#include "dxDebugRender.h"
#include "OGSRHiZ.h"
#include "FVisual.h"
#include "dxRenderDeviceRender.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <memory>
#include <vector>
#include <map>
#include <set>

// The supplied Layers archives contain no Intel MOC implementation.
// Enable this macro when the vendor library and its header are installed.
#if defined(OGSR_USE_INTEL_MOC)
#include "MaskedOcclusionCulling.h"
#endif

namespace
{
    constexpr unsigned MOC_WIDTH = 256;
    constexpr unsigned MOC_HEIGHT = 144;
    constexpr u32 MOC_CACHE_VERSION = 14;
    constexpr u32 MOC_CACHE_MAGIC = 0x434F4D58u;
    constexpr float MOC_CLUSTER_SIZE = 16.f;
#pragma pack(push,1)
    struct CacheHeader
    {
        u32 magic, version, vertexCount, sourceTriangleCount, occluderTriangleCount, clusterCount;
        u64 hash;
        float clusterSize;
    };
    struct CacheCluster
    {
        float minx, miny, minz, maxx, maxy, maxz;
        u32 indexCount;
    };
#pragma pack(pop)
    struct CapturedVB
    {
        xr_vector<BYTE> data;
        u32 stride = 0;
        u16 offset = 0;
        u8 type = D3DDECLTYPE_UNUSED;
    };
    xr_vector<CapturedVB> s_vb;
    xr_vector<xr_vector<u16>> s_ib;
    u64 HashBytes(u64 hash, const void* ptr, size_t bytes)
    {
        auto b = static_cast<const BYTE*>(ptr);
        while (bytes--) { hash ^= *b++; hash *= 1099511628211ULL; }
        return hash;
    }
    bool CachePath(string_path& path, bool create)
    {
        if (!g_pGameLevel) return false;
        string_path dir;
        FS.update_path(dir, "$app_data_root$", "moc_cache");
        if (create && !CreateDirectoryA(dir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
        string128 level;
        xr_strcpy(level, g_pGameLevel->name().c_str());
        for (char* c = level; *c; ++c)
            if (strchr("\\/:*?\"<>|", *c)) *c = '_';
        string_path name;
        xr_sprintf(name, sizeof(name), "moc_cache\\%s.xmoc", level);
        FS.update_path(path, "$app_data_root$", name);
        return true;
    }
    bool OpaqueVisual(dxRender_Visual* v)
    {
        if (!v || !v->shader) return false;
        for (u32 i=0; i<2; ++i)
        {
            const ref_selement& e = v->shader->E[i];
            if (!e) continue;
            if (e->flags.bStrictB2F || e->flags.bDistort) return false;
            for (const auto& pass : e->passes)
            {
                if (!pass) continue;
                if (pass->ps)
                {
                    LPCSTR n = pass->ps->cName.c_str();
                    // Conservative rejection of common cutout shaders.
                    // OGSR's additional alpha paths should be audited separately.
                    if (strstr(n,"aref") || strstr(n,"atoc") || strstr(n,"alpha")) return false;
                }
                if (pass->state)
                {
                    u32 v = 0;
                    if (pass->state->state_code.get_RS(D3DRS_ALPHABLENDENABLE, v) && v) return false;
                    if (pass->state->state_code.get_RS(D3DRS_ALPHATESTENABLE, v) && v) return false;
                }
            }
        }
        return true;
    }
    struct GridKey
    {
        s32 x,y,z;
        bool operator<(const GridKey& o) const
        { if (x!=o.x) return x<o.x; if(y!=o.y) return y<o.y; return z<o.z; }
    };
    struct VertexKey
    {
        u32 x, y, z;
        explicit VertexKey(const Fvector& v)
        {
            static_assert(sizeof(float) == sizeof(u32), "Unexpected float size");
            memcpy(&x, &v.x, sizeof(x));
            memcpy(&y, &v.y, sizeof(y));
            memcpy(&z, &v.z, sizeof(z));
            // Canonicalize negative zero without welding distinct coordinates.
            if ((x << 1u) == 0u) x = 0u;
            if ((y << 1u) == 0u) y = 0u;
            if ((z << 1u) == 0u) z = 0u;
        }
        bool operator<(const VertexKey& o) const
        { if (x != o.x) return x < o.x; if (y != o.y) return y < o.y; return z < o.z; }
    };
    struct TriangleKey
    {
        u32 v[3];
        bool operator<(const TriangleKey& o) const
        { for(int k=0;k<3;++k) if(v[k]!=o.v[k]) return v[k]<o.v[k]; return false; }
    };
#pragma pack(push,4)
    struct SerializedFace { Fvector v1, v2, v3; u32 flags; };
#pragma pack(pop)
    // The old authored HOM format and the OGSR HOM reader use this same layout.
    bool ProjectBox(const Fbox& B, const Fmatrix& X,
        float& xmin, float& ymin, float& xmax, float& ymax, float& wmin)
    {
        xmin = ymin = flt_max;
        xmax = ymax = -flt_max;
        wmin = flt_max;
        for (unsigned i = 0; i < 8; ++i)
        {
            const float x = (i & 1) ? B.max.x : B.min.x;
            const float y = (i & 2) ? B.max.y : B.min.y;
            const float z = (i & 4) ? B.max.z : B.min.z;
            const float cx = x*X._11 + y*X._21 + z*X._31 + X._41;
            const float cy = x*X._12 + y*X._22 + z*X._32 + X._42;
            const float cw = x*X._14 + y*X._24 + z*X._34 + X._44;
            // Clip-space intersection/near plane: never reject such an object.
            if (cw <= VIEWPORT_NEAR + EPS_L) return false;
            const float nx = cx/cw, ny = cy/cw;
            xmin = std::min(xmin, nx); ymin = std::min(ymin, ny);
            xmax = std::max(xmax, nx); ymax = std::max(ymax, ny);
            wmin = std::min(wmin, cw);
        }
        // Conservative sub-pixel guard (rounding / jitter).
        constexpr float guard = 1e-4f;
        xmin -= guard; ymin -= guard; xmax += guard; ymax += guard;
        return true;
    }
}

struct COGSRMOC::Snapshot
{
    u32 frame = u32(-1);
    Fmatrix full;
    Fvector camera;
    u32 submittedTriangles = 0;
    u32 hizProbed = 0, hizHidden = 0, candidates = 0;
    bool hizSelection = false;
    bool generated = false;
    // Debug mode 2: all source clusters accepted by Hi-Z / frustum.
    xr_vector<u32> hizVisibleAutoClusters;
    xr_vector<u32> hizVisibleFaceIds;
#if defined(OGSR_USE_INTEL_MOC)
    MaskedOcclusionCulling* context = nullptr;
    ~Snapshot() { if (context) MaskedOcclusionCulling::Destroy(context); }
#endif
};

void OGSRMOC_CaptureVertexBuffer(u32 id, const BYTE* bytes, u32 size, u32 stride, u16 offset, u8 type)
{
#if defined(OGSR_USE_INTEL_MOC)
    if (s_vb.size() <= id) s_vb.resize(id+1);
    CapturedVB& v = s_vb[id];
    v.stride=stride; v.offset=offset; v.type=type;
    v.data.assign(bytes,bytes+size);
#else
    (void)id; (void)bytes; (void)size; (void)stride; (void)offset; (void)type;
#endif
}
void OGSRMOC_CaptureIndexBuffer(u32 id, const u16* indices, u32 count)
{
#if defined(OGSR_USE_INTEL_MOC)
    if (s_ib.size() <= id) s_ib.resize(id+1);
    s_ib[id].assign(indices,indices+count);
#else
    (void)id; (void)indices; (void)count;
#endif
}
void OGSRMOC_ClearGeometryCapture()
{
    xr_vector<CapturedVB>().swap(s_vb);
    xr_vector<xr_vector<u16>>().swap(s_ib);
    // Standalone OGF payloads are only required during cache generation.
    // Reclaim them after the MOC geometry (or cached version) has loaded.
    for (dxRender_Visual* visual : RImplementation.Visuals)
    {
        Fvisual* mesh = dynamic_cast<Fvisual*>(visual);
        if (!mesh) continue;
        xr_vector<Fvector>().swap(mesh->m_mocLocalVertices);
        xr_vector<u16>().swap(mesh->m_mocLocalIndices);
    }
}

u64 COGSRMOC::ComputeSourceHash() const
{
    u64 h = 1469598103934665603ULL;
    h = HashBytes(h, &MOC_CACHE_VERSION, sizeof(MOC_CACHE_VERSION));
    const u32 vbCount = (u32)s_vb.size(), ibCount = (u32)s_ib.size();
    h = HashBytes(h, &vbCount, 4);
    for (const auto& vb : s_vb)
    {
        u32 n=(u32)vb.data.size();
        h=HashBytes(h,&n,4);
        if (n) h=HashBytes(h,vb.data.data(),n);
    }
    h=HashBytes(h,&ibCount,4);
    for (const auto& ib : s_ib)
    {
        u32 n=(u32)ib.size();
        h=HashBytes(h,&n,4);
        if (n) h=HashBytes(h,ib.data(),n*sizeof(u16));
    }
    // Include material opacity so a new alpha fence cannot re-use an old cache.
    const u32 nVisuals=(u32)RImplementation.Visuals.size();
    h=HashBytes(h,&nVisuals,4);
    for (dxRender_Visual* visual : RImplementation.Visuals)
    {
        u32 kind=visual ? visual->Type : u32(-1);
        u32 opaque=OpaqueVisual(visual)?1u:0u;
        h=HashBytes(h,&kind,4); h=HashBytes(h,&opaque,4);
        const auto* mesh=dynamic_cast<const Fvisual*>(visual);
        if(mesh)
        {
            const u32 source[6]={mesh->m_mocSourceVB,mesh->m_mocSourceIB,
                mesh->m_mocVBase,mesh->m_mocVCount,mesh->m_mocIBase,mesh->m_mocICount};
            h=HashBytes(h,source,sizeof(source));
            const u32 localVerts=(u32)mesh->m_mocLocalVertices.size();
            const u32 localIndices=(u32)mesh->m_mocLocalIndices.size();
            h=HashBytes(h,&localVerts,sizeof(localVerts));
            h=HashBytes(h,&localIndices,sizeof(localIndices));
            if(localVerts) h=HashBytes(h,mesh->m_mocLocalVertices.data(),size_t(localVerts)*sizeof(Fvector));
            if(localIndices) h=HashBytes(h,mesh->m_mocLocalIndices.data(),size_t(localIndices)*sizeof(u16));
        }
    }
    return h;
}

bool COGSRMOC::ReadCache(u64 hash)
{
    if (strstr(Core.Params,"-no_moc_cache") || strstr(Core.Params,"-moc_rebuild_cache")) return false;
    string_path path;
    if (!CachePath(path,false) || !FS.exist(path)) return false;
    IReader* r=FS.r_open(path); if(!r) return false;
    bool good=false;
    do
    {
        if (r->length()<sizeof(CacheHeader)) break;
        CacheHeader h; r->r(&h,sizeof(h));
        if (h.magic!=MOC_CACHE_MAGIC || h.version!=MOC_CACHE_VERSION || h.hash!=hash ||
            h.clusterSize!=MOC_CLUSTER_SIZE || !h.vertexCount || !h.clusterCount ||
            !h.sourceTriangleCount || h.occluderTriangleCount>h.sourceTriangleCount ||
            h.occluderTriangleCount<h.clusterCount) break;
        // Reject truncated/corrupt files by their ACTUAL byte count, never by
        // arbitrary numbers of triangles, vertices, or clusters.
        if (u64(h.vertexCount)*sizeof(Fvector)>r->elapsed()) break;
        // At least one triangle (three 32-bit indices) per nonempty cluster.
        const u64 afterVertices=u64(r->elapsed())-u64(h.vertexCount)*sizeof(Fvector);
        if (u64(h.clusterCount)*(sizeof(CacheCluster)+3u*sizeof(u32))>afterVertices) break;
        m_autoVertices.resize(h.vertexCount);
        r->r(m_autoVertices.data(),h.vertexCount*sizeof(Fvector));
        m_autoClusters.clear(); m_autoClusters.reserve(h.clusterCount);
        u64 sum=0;
        for(u32 i=0;i<h.clusterCount;++i)
        {
            if (r->elapsed()<sizeof(CacheCluster)) break;
            CacheCluster c; r->r(&c,sizeof(c));
            if (!c.indexCount || (c.indexCount%3u) || c.indexCount>h.sourceTriangleCount*3ull ||
                (u64)c.indexCount*4>r->elapsed()) break;
            sum+=c.indexCount;
            if (sum>u64(h.sourceTriangleCount)*3u) break;
            Cluster cluster;
            cluster.box.min.set(c.minx,c.miny,c.minz);
            cluster.box.max.set(c.maxx,c.maxy,c.maxz);
            cluster.box.getcenter(cluster.center);
            cluster.indices.resize(c.indexCount);
            r->r(cluster.indices.data(),c.indexCount*4u);
            bool valid=true;
            for(u32 idx:cluster.indices) if(idx>=h.vertexCount) {valid=false;break;}
            if (!valid) break;
            m_autoClusters.push_back(std::move(cluster));
        }
        if (m_autoClusters.size()!=static_cast<size_t>(h.clusterCount) || sum/3u!=h.occluderTriangleCount) break;
        m_autoInputTriangles=h.sourceTriangleCount;
        good=true;
    }while(false);
    FS.r_close(r);
    if(!good) {m_autoVertices.clear();m_autoClusters.clear();}
    else Msg("* OGSR MOC cache: HIT, %u clusters",(u32)m_autoClusters.size());
    return good;
}

void COGSRMOC::WriteCache(u64 hash)
{
    if (strstr(Core.Params,"-no_moc_cache") || m_autoClusters.empty()) return;
    string_path path;
    if(!CachePath(path,true)) return;
    string_path temp; xr_sprintf(temp,sizeof(temp),"%s.tmp",path);
    IWriter* w=FS.w_open(temp); if(!w) return;
    CacheHeader h{};
    h.magic=MOC_CACHE_MAGIC;h.version=MOC_CACHE_VERSION;
    h.hash=hash;h.vertexCount=(u32)m_autoVertices.size();
    h.sourceTriangleCount=m_autoInputTriangles;
    h.clusterCount=(u32)m_autoClusters.size();h.clusterSize=MOC_CLUSTER_SIZE;
    for (const auto& c:m_autoClusters) h.occluderTriangleCount+=(u32)c.indices.size()/3u;
    w->w(&h,sizeof(h));w->w(m_autoVertices.data(),h.vertexCount*sizeof(Fvector));
    for(const auto& c:m_autoClusters)
    {
        CacheCluster k{c.box.min.x,c.box.min.y,c.box.min.z,c.box.max.x,c.box.max.y,c.box.max.z,(u32)c.indices.size()};
        w->w(&k,sizeof(k));w->w(c.indices.data(),k.indexCount*4);
    }
    FS.w_close(w);
    if(!MoveFileExA(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) DeleteFileA(temp);
    else Msg("* OGSR MOC cache: SAVED %s",path);
}

bool COGSRMOC::LoadAuto()
{
    m_autoReady = false;
    m_autoClusters.clear(); m_autoVertices.clear(); m_autoInputTriangles = 0;
    // level.geom and standalone OGF_VERTICES visuals can coexist. The latter
    // are still valid source data even when no global geometry buffers exist.
    if (s_vb.empty() || s_ib.empty())
        Msg("! OGSR MOC AutoHOM: level.geom capture missing; scanning standalone visual geometry");
    const u64 hash = ComputeSourceHash();
    if (ReadCache(hash)) return m_autoReady = true;

    std::map<GridKey,u32> grids;
    std::map<VertexKey,u32> weldedVertices;
    std::set<TriangleKey> uniqueTriangles;
    u32 leaf = 0, accepted = 0, alpha = 0, invalidSource = 0;
    u32 badVertexFormat = 0, duplicate = 0, degenerate = 0, nonLeaf = 0;
    // Process the complete level. The only remaining bound comes from the
    // 32-bit index/count representation in the .xmoc format / Intel MOC API.
    // On a representation overflow, discard the entire model; never cache
    // a partially generated AutoHOM.
    bool overflow = false;
    for (dxRender_Visual* visual : RImplementation.Visuals)
    {
        if (!visual || (visual->Type != MT_NORMAL && visual->Type != MT_PROGRESSIVE))
        { ++nonLeaf; continue; }
        ++leaf;
        if (!OpaqueVisual(visual)) { ++alpha; continue; }
        const auto* mesh = dynamic_cast<const Fvisual*>(visual);
        if (!mesh) { ++invalidSource; continue; }
        const bool local = !mesh->m_mocLocalVertices.empty() && !mesh->m_mocLocalIndices.empty();
        const bool global = mesh->m_mocSourceVB < s_vb.size() && mesh->m_mocSourceIB < s_ib.size();
        if (!local && !global) { ++invalidSource; continue; }
        const CapturedVB* vb = global ? &s_vb[mesh->m_mocSourceVB] : nullptr;
        const xr_vector<u16>& ib = local ? mesh->m_mocLocalIndices : s_ib[mesh->m_mocSourceIB];
        const u32 vertCount = local ? (u32)mesh->m_mocLocalVertices.size() : mesh->m_mocVCount;
        const u32 iBase = local ? 0u : mesh->m_mocIBase;
        const u32 iCount = local ? (u32)ib.size() : mesh->m_mocICount;
        if (!local && (vb->type != D3DDECLTYPE_FLOAT3 || vb->stride == 0 ||
            vb->stride < u32(vb->offset) + u32(sizeof(Fvector))))
        { ++badVertexFormat; continue; }
        if ((!local && ((u64(mesh->m_mocVBase)+vertCount)*vb->stride > vb->data.size())) ||
            u64(iBase)+iCount > ib.size())
        { ++invalidSource; continue; }
        ++accepted;
        for (u32 k=0; k+2<iCount; k+=3)
        {
            if (m_autoInputTriangles == u32(-1)) { overflow=true; break; }
            ++m_autoInputTriangles;
            const u32 base = iBase + k;
            const u16 ids[3] = {ib[base],ib[base+1],ib[base+2]};
            if (ids[0] >= vertCount || ids[1] >= vertCount || ids[2] >= vertCount)
            { ++invalidSource; continue; }
            Fvector tri[3];
            for (int q=0; q<3; ++q)
            {
                if (local)
                    tri[q] = mesh->m_mocLocalVertices[ids[q]];
                else
                {
                    const BYTE* ptr = vb->data.data() +
                        (u64(mesh->m_mocVBase) + ids[q]) * vb->stride + vb->offset;
                    memcpy(&tri[q],ptr,sizeof(Fvector));
                }
            }
            Fvector e1,e2,normal;
            e1.sub(tri[1],tri[0]); e2.sub(tri[2],tri[0]); normal.crossproduct(e1,e2);
            if (normal.square_magnitude() < EPS_L*EPS_L) { ++degenerate; continue; }

            u32 indices[3];
            for (u32 q=0; q<3; ++q)
            {
                const VertexKey key(tri[q]);
                auto it = weldedVertices.find(key);
                if (it == weldedVertices.end())
                {
                    if (m_autoVertices.size() >= size_t(u32(-1)))
                    { overflow=true; break; }
                    const u32 n = (u32)m_autoVertices.size();
                    m_autoVertices.push_back(tri[q]);
                    weldedVertices.emplace(key,n);
                    indices[q]=n;
                }
                else indices[q]=it->second;
            }
            if (overflow) break;
            TriangleKey key{{indices[0],indices[1],indices[2]}};
            std::sort(key.v,key.v+3);
            if (!uniqueTriangles.insert(key).second) { ++duplicate; continue; }

            Fvector center; center.add(tri[0],tri[1]).add(tri[2]).div(3.f);
            GridKey grid{iFloor(center.x/MOC_CLUSTER_SIZE),
                iFloor(center.y/MOC_CLUSTER_SIZE),iFloor(center.z/MOC_CLUSTER_SIZE)};
            auto loc=grids.find(grid);
            u32 ci;
            if (loc==grids.end())
            {
                if (m_autoClusters.size() >= size_t(u32(-1)))
                { overflow=true; break; }
                ci=(u32)m_autoClusters.size(); grids.emplace(grid,ci);
                m_autoClusters.emplace_back();
            }
            else ci=loc->second;
            auto& c=m_autoClusters[ci];
            c.indices.insert(c.indices.end(),indices,indices+3);
            c.box.modify(tri[0]); c.box.modify(tri[1]); c.box.modify(tri[2]);
        }
        if (overflow) break;
    }
    if (overflow)
    {
        Msg("! OGSR MOC AutoHOM: 32-bit index/count representation exhausted; falling back to authored HOM (no partial cache)");
        m_autoVertices.clear(); m_autoClusters.clear(); m_autoInputTriangles=0;
        return false;
    }
    if (m_autoClusters.empty())
    {
        m_autoVertices.clear();
        Msg("! OGSR MOC AutoHOM: no opaque render meshes; fallback to level.hom");
        return false;
    }
    for (auto& c:m_autoClusters) c.box.getcenter(c.center);
    m_autoReady=true;
    u32 kept=0;
    for (const auto& c:m_autoClusters) kept += (u32)c.indices.size()/3;
    Msg("* OGSR MOC AutoHOM v11 (uncapped): source=%u kept=%u unique_verts=%u clusters=%u duplicates=%u degenerate=%u",
        m_autoInputTriangles,kept,(u32)m_autoVertices.size(),(u32)m_autoClusters.size(),duplicate,degenerate);
    Msg("* OGSR MOC visuals: leaf=%u accepted=%u alpha=%u invalid_source=%u bad_format=%u non_leaf=%u",
        leaf,accepted,alpha,invalidSource,badVertexFormat,nonLeaf);
    WriteCache(hash);
    return true;
}

// Keep the snapshot lifetime safe for TTAPI readers. In C++20 the old
// std::atomic_load(shared_ptr*) overloads produce MSVC STL4029/C4996.
// C++17 remains supported without a global warning suppression.
std::shared_ptr<const COGSRMOC::Snapshot> COGSRMOC::LoadSnapshot() const
{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return m_snapshot.load(std::memory_order_acquire);
#else
    return std::atomic_load_explicit(&m_snapshot, std::memory_order_acquire);
#endif
}

std::shared_ptr<const COGSRMOC::Snapshot> COGSRMOC::ExchangeSnapshot(
    std::shared_ptr<const Snapshot> next)
{
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    return m_snapshot.exchange(std::move(next), std::memory_order_acq_rel);
#else
    return std::atomic_exchange_explicit(&m_snapshot, std::move(next), std::memory_order_acq_rel);
#endif
}

COGSRMOC::~COGSRMOC() { Unload(); }

bool COGSRMOC::Load()
{
    Unload();
#if !defined(OGSR_USE_INTEL_MOC)
    Msg("! OGSR MOC: Intel source absent; using original OGSR HOM. Define OGSR_USE_INTEL_MOC and add Intel's library to the project.");
    OGSRMOC_ClearGeometryCapture();
    return false;
#else
    string_path path;
    FS.update_path(path, "$level$", "level.hom");
    if (!FS.exist(path))
    {
        m_autoReady = LoadAuto();
        OGSRMOC_ClearGeometryCapture();
        m_sourceLoaded = m_autoReady;
        return m_sourceLoaded;
    }
    IReader* reader = FS.r_open(path);
    if (!reader) return false;
    IReader* chunk = reader->open_chunk(1);
    if (!chunk) { FS.r_close(reader); return false; }
    const u32 nBytes = chunk->length();
    const u32 nFaces = nBytes / sizeof(SerializedFace);
    if (!nFaces || nBytes % sizeof(SerializedFace))
    {
        chunk->close(); FS.r_close(reader);
        m_autoReady = LoadAuto();
        OGSRMOC_ClearGeometryCapture();
        m_sourceLoaded = m_autoReady;
        return m_sourceLoaded;
    }
    m_faces.reserve(nFaces);
    m_vertices.reserve(size_t(nFaces) * 3u);
    for (u32 i=0; i<nFaces; ++i)
    {
        SerializedFace s{};
        chunk->r(&s, sizeof(s));
        Fvector edge1, edge2, cross;
        edge1.sub(s.v2,s.v1); edge2.sub(s.v3,s.v1);
        cross.crossproduct(edge1,edge2);
        if (cross.square_magnitude() <= EPS_L * EPS_L) continue;
        Face f;
        f.box.invalidate();
        f.box.modify(s.v1); f.box.modify(s.v2); f.box.modify(s.v3);
        f.plane.build(s.v1,s.v2,s.v3);
        f.center.add(s.v1,s.v2).add(s.v3).div(3.f);
        f.flags = s.flags;
        m_faces.push_back(f);
        m_vertices.push_back(s.v1);
        m_vertices.push_back(s.v2);
        m_vertices.push_back(s.v3);
    }
    chunk->close(); FS.r_close(reader);
    m_autoReady = LoadAuto();
    OGSRMOC_ClearGeometryCapture(); // Keep only compact occluder geometry after level load.
    m_sourceLoaded = !m_faces.empty() || m_autoReady;
    Msg("* OGSR MOC: authored HOM faces %u, level %s", (u32)m_faces.size(), path);
    return m_sourceLoaded;
#endif
}

void COGSRMOC::Unload()
{
    ExchangeSnapshot(std::shared_ptr<const Snapshot>{});
    m_spare.reset();
    m_sourceLoaded = false;
    m_faces.clear(); m_vertices.clear();
    m_autoReady=false; m_autoInputTriangles=0; m_autoVertices.clear(); m_autoClusters.clear();
    m_tests.store(0); m_rejected.store(0);
}

void COGSRMOC::Build(CFrustum& frustum)
{
#if defined(OGSR_USE_INTEL_MOC)
    if (!m_sourceLoaded || !ps_r__moc_enable) return;
    std::shared_ptr<Snapshot> next;
    if (m_spare && m_spare.use_count() == 1)
        next = std::move(m_spare);
    else
        next = std::make_shared<Snapshot>();
    next->submittedTriangles = 0;
    next->hizProbed=next->hizHidden=next->candidates=0;
    next->hizSelection=false;
    next->hizVisibleAutoClusters.clear();
    next->hizVisibleFaceIds.clear();
    next->frame = Device.dwFrame;
    next->full = Device.mFullTransform;
    next->camera = Device.vCameraPosition;
    if (!next->context)
    {
        next->context = MaskedOcclusionCulling::Create();
        if (!next->context) return;
        next->context->SetResolution(MOC_WIDTH, MOC_HEIGHT);
    }
    next->context->SetNearClipPlane(VIEWPORT_NEAR);
    next->context->ClearBuffer();
    // CPU occlusion raster budget, applied to both AutoHOM and authored HOM.
    // Not a limit on geometry generation or the contents of .xmoc.
    constexpr u32 kRasterTriangleBudget = 12000u;
    next->generated = (ps_r__moc_source == 1 && m_autoReady) || m_faces.empty();
    // True MOC/Hi-Z integration: pin one immutable GPU depth snapshot BEFORE
    // ranking CPU MOC occluders. If it is missing/stale/camera-incompatible,
    // run the entire original frustum-only path (fail open).
    // Automatically use Hi-Z occluder selection in BOTH modes 1 and 2.
    // Missing/stale snapshots fail open to frustum-only MOC selection.
    // Mode 1 does NOT independently cull scene objects; mode 2 does.
    const OGSRHiZ::OccluderSelection hiz = OGSRHiZ::BeginOccluderSelection(
        next->full, next->camera, Device.vCameraDirection, next->frame);
    next->hizSelection = hiz.enabled;
    if (next->generated && m_autoReady)
    {
        struct CandidateAuto
        {
            u32 id;
            float score;
            float distanceSq;
        };
        std::vector<CandidateAuto> chosen;
        chosen.reserve(m_autoClusters.size());
        for (size_t i=0; i<m_autoClusters.size(); ++i)
        {
            const auto& c=m_autoClusters[i];
            u32 mask=frustum.getMask();
            if (fcvNone==frustum.testAABB(&c.box.min.x,mask)) continue;
            ++next->candidates;
            if (hiz.enabled)
            {
                ++next->hizProbed;
                if (OGSRHiZ::OccludedForOccluder(hiz,c.box))
                { ++next->hizHidden; continue; }
            }
            // Prioritise large projected occluders for early depth coverage,
            // but do not discard any accepted triangles.
            const float d=next->camera.distance_to_sqr(c.center);
            float xmin,ymin,xmax,ymax,nearW;
            float coverage=0.0001f;
            if (ProjectBox(c.box,next->full,xmin,ymin,xmax,ymax,nearW))
            {
                const float left=std::max(-1.f,xmin),right=std::min(1.f,xmax);
                const float bottom=std::max(-1.f,ymin),top=std::min(1.f,ymax);
                if (right>left && top>bottom)
                    coverage=std::max(coverage,(right-left)*(top-bottom));
            }
            const float score=coverage / (1.f+0.015f*std::sqrt(d));
            chosen.push_back({static_cast<u32>(i),score,d});
            if (ps_r__moc_draw == 2)
                next->hizVisibleAutoClusters.push_back(static_cast<u32>(i));
        }
        std::sort(chosen.begin(),chosen.end(),[](const CandidateAuto& a,const CandidateAuto& b)
        {
            if (a.score!=b.score) return a.score>b.score;
            if (a.distanceSq!=b.distanceSq) return a.distanceSq<b.distanceSq;
            return a.id<b.id;
        });
        std::vector<u32> batch;
        batch.reserve(384u*3u);
        MaskedOcclusionCulling::VertexLayout layout(sizeof(Fvector),4,8);
        const auto flush=[&]()
        {
            if (batch.empty()) return;
            next->context->RenderTriangles(reinterpret_cast<const float*>(m_autoVertices.data()),
                batch.data(),(int)(batch.size()/3u),&next->full._11,
                MaskedOcclusionCulling::BACKFACE_NONE,MaskedOcclusionCulling::CLIP_PLANE_ALL,layout);
            next->submittedTriangles += static_cast<u32>(batch.size()/3u);
            batch.clear();
        };
        // Keep screen-priority order, but only feed 12K triangles to CPU MOC.
        // A cluster that crosses the budget is partially submitted, never
        // allowed to overrun it. Hi-Z debug selection remains independent.
        for (const auto& candidate : chosen)
        {
            const auto& cluster = m_autoClusters[candidate.id];
            for (size_t j = 0; j + 2u < cluster.indices.size(); j += 3u)
            {
                if (next->submittedTriangles + batch.size()/3u >= kRasterTriangleBudget)
                    break;
                batch.push_back(cluster.indices[j]);
                batch.push_back(cluster.indices[j+1u]);
                batch.push_back(cluster.indices[j+2u]);
                if (batch.size() >= 384u*3u) flush();
            }
            if (next->submittedTriangles + batch.size()/3u >= kRasterTriangleBudget)
                break;
        }
        flush();
        const auto old=ExchangeSnapshot(std::shared_ptr<const Snapshot>(std::move(next)));
        m_spare=std::const_pointer_cast<Snapshot>(old);
        return;
    }
    struct Candidate
    {
        u32 index; float distance;
        bool operator<(const Candidate& rhs) const { return distance < rhs.distance; }
    };
    std::vector<Candidate> candidates;
    candidates.reserve(m_faces.size());
    for (size_t i = 0; i < m_faces.size(); ++i)
    {
        const Face& f = m_faces[i];
        u32 mask = frustum.getMask();
        if (fcvNone == frustum.testAABB(&f.box.min.x, mask)) continue;
        if (!f.flags && f.plane.classify(next->camera) <= 0.f) continue;
        ++next->candidates;
        if (hiz.enabled)
        {
            ++next->hizProbed;
            if (OGSRHiZ::OccludedForOccluder(hiz,f.box))
            { ++next->hizHidden; continue; }
        }
        candidates.push_back({static_cast<u32>(i), next->camera.distance_to_sqr(f.center)});
        if (ps_r__moc_draw == 2)
            next->hizVisibleFaceIds.push_back(static_cast<u32>(i));
    }
    std::sort(candidates.begin(), candidates.end());
    // Authored level.hom uses the same 12K CPU budget as AutoHOM.
    const size_t submittedCount = std::min(candidates.size(),
        static_cast<size_t>(kRasterTriangleBudget));
    std::vector<u32> indices;
    indices.reserve(384 * 3);
    MaskedOcclusionCulling::VertexLayout layout(sizeof(Fvector),4,8);
    for (size_t i=0; i<submittedCount; ++i)
    {
        const u32 index = candidates[i].index * 3u;
        indices.push_back(index);
        indices.push_back(index+1);
        indices.push_back(index+2);
        if (indices.size() < 384u*3u && i+1<submittedCount) continue;
        next->context->RenderTriangles(reinterpret_cast<const float*>(m_vertices.data()),
            indices.data(), (int)(indices.size()/3), &next->full._11,
            MaskedOcclusionCulling::BACKFACE_NONE,
            MaskedOcclusionCulling::CLIP_PLANE_ALL, layout);
        next->submittedTriangles += (u32)(indices.size()/3);
        indices.clear();
    }
    // Build and query cannot overlap against the same context. Publishing only
    // happens after all rasterization is complete. Readers retain old snapshots.
    const std::shared_ptr<const Snapshot> old = ExchangeSnapshot(
        std::shared_ptr<const Snapshot>(std::move(next)));
    // Other workers may still hold 'old'; use_count protects it from reuse.
    m_spare = std::const_pointer_cast<Snapshot>(old);
#endif
}

bool COGSRMOC::Active() const
{
#if defined(OGSR_USE_INTEL_MOC)
    return m_sourceLoaded && ps_r__moc_enable;
#else
    return false;
#endif
}

BOOL COGSRMOC::TestRect(float xmin,float ymin,float xmax,float ymax,float clipW, const std::shared_ptr<const Snapshot>& snap) const
{
#if defined(OGSR_USE_INTEL_MOC)
    if (!snap || snap->frame != Device.dwFrame || !ps_r__moc_enable ||
        memcmp(&snap->full, &Device.mFullTransform, sizeof(Fmatrix)) != 0) return TRUE;
    if (clipW <= VIEWPORT_NEAR + EPS_L) return TRUE;
    if (xmax <= -1.f || xmin >= 1.f || ymax <= -1.f || ymin >= 1.f) return TRUE;
    xmin = std::max(-1.f, xmin); xmax = std::min(1.f, xmax);
    ymin = std::max(-1.f, ymin); ymax = std::min(1.f, ymax);
    if (xmin >= xmax || ymin >= ymax) return TRUE;
    const bool occluded = snap->context->TestRect(xmin,ymin,xmax,ymax,clipW) == MaskedOcclusionCulling::OCCLUDED;
    m_tests.fetch_add(1, std::memory_order_relaxed);
    if (occluded) m_rejected.fetch_add(1, std::memory_order_relaxed);
    // We intentionally don't mutate per-object visibility/renderer caches here.
    // OGSR workers may call this concurrently in static/dynamic workloads.
    if (occluded) return FALSE;
#endif
    return TRUE;
}

BOOL COGSRMOC::Visible(const Fbox& B) const
{
#if defined(OGSR_USE_INTEL_MOC)
    auto snap = LoadSnapshot();
    if (!snap || snap->frame != Device.dwFrame ||
        memcmp(&snap->full, &Device.mFullTransform, sizeof(Fmatrix)) != 0) return TRUE;
    if (B.contains(snap->camera)) return TRUE;
    float xmin,ymin,xmax,ymax,clipW;
    if (!ProjectBox(B,snap->full,xmin,ymin,xmax,ymax,clipW)) return TRUE;
    return TestRect(xmin,ymin,xmax,ymax,clipW,snap);
#else
    return TRUE;
#endif
}

BOOL COGSRMOC::VisiblePadded(const Fbox& B, float padPixels) const
{
#if defined(OGSR_USE_INTEL_MOC)
    auto snap=LoadSnapshot();
    if (!snap || snap->frame != Device.dwFrame || B.contains(snap->camera) ||
        memcmp(&snap->full, &Device.mFullTransform, sizeof(Fmatrix)) != 0) return TRUE;
    float xmin,ymin,xmax,ymax,w;
    if(!ProjectBox(B,snap->full,xmin,ymin,xmax,ymax,w)) return TRUE;
    const float px=std::max(0.f,padPixels);
    xmin-=2.f*px/MOC_WIDTH;xmax+=2.f*px/MOC_WIDTH;
    ymin-=2.f*px/MOC_HEIGHT;ymax+=2.f*px/MOC_HEIGHT;
    return TestRect(xmin,ymin,xmax,ymax,w,snap);
#else
    return TRUE;
#endif
}

BOOL COGSRMOC::Visible(const sPoly& poly) const
{
#if defined(OGSR_USE_INTEL_MOC)
    auto snap = LoadSnapshot();
    if (!snap || snap->frame != Device.dwFrame || poly.empty() ||
        memcmp(&snap->full, &Device.mFullTransform, sizeof(Fmatrix)) != 0) return TRUE;
    float xmin=flt_max, ymin=flt_max, xmax=-flt_max, ymax=-flt_max, clipW=flt_max;
    const Fmatrix& X = snap->full;
    for (const Fvector& p : poly)
    {
        float cx=p.x*X._11 + p.y*X._21 + p.z*X._31 + X._41;
        float cy=p.x*X._12 + p.y*X._22 + p.z*X._32 + X._42;
        float cw=p.x*X._14 + p.y*X._24 + p.z*X._34 + X._44;
        if (cw <= VIEWPORT_NEAR + EPS_L) return TRUE;
        xmin=std::min(xmin,cx/cw); xmax=std::max(xmax,cx/cw);
        ymin=std::min(ymin,cy/cw); ymax=std::max(ymax,cy/cw);
        clipW=std::min(clipW,cw);
    }
    return TestRect(xmin,ymin,xmax,ymax,clipW,snap);
#else
    return TRUE;
#endif
}

void COGSRMOC::Stats(CGameFont& font) const
{
    auto snap = LoadSnapshot();
    font.OutNext("OGSR MOC: enabled %u / loaded %u / published frame %u / raster tris %u",
        (u32)Active(), (u32)m_sourceLoaded, snap ? snap->frame : 0u,
        snap ? snap->submittedTriangles : 0u);
    font.OutNext("OGSR MOC: tests %u / rejected %u", m_tests.load(), m_rejected.load());
    if (snap)
        font.OutNext("OGSR MOC < HiZ: %s | frustum %u, GPU tests %u, hidden %u, raster %u",
            snap->hizSelection ? "ACTIVE" : "FALLBACK", snap->candidates,
            snap->hizProbed, snap->hizHidden, snap->submittedTriangles);
}

void COGSRMOC::DebugDraw() const
{
#ifdef DEBUG
    if (!Active() || !ps_r__moc_draw)
        return;
    const auto snap = LoadSnapshot();
    if (!snap || snap->frame != Device.dwFrame)
        return;
    // Draw only with the legacy debug rendering path. Never modify live depth.
    const u32 color = ps_r__moc_draw == 2 ? 0xFF60FF20u : 0xFF00DF40u;
    xr_vector<FVF::L> batch;
    batch.reserve(3u * 384u);
    auto flush = [&]() {
        if (batch.empty()) return;
        RCache.set_xform_world(Fidentity);
        RCache.set_Shader(dxRenderDeviceRender::Instance().m_SelectionShader);
        RCache.set_c("tfactor", float(color_get_R(color)) / 255.f,
                     float(color_get_G(color)) / 255.f, float(color_get_B(color)) / 255.f, 1.f);
        RCache.dbg_Draw(D3DPT_TRIANGLELIST, batch.data(), (u32)(batch.size() / 3));
        batch.clear();
    };
    auto add = [&](const Fvector& a, const Fvector& b, const Fvector& c) {
        FVF::L v;
        v.color = color;
        v.p = a; batch.push_back(v);
        v.p = b; batch.push_back(v);
        v.p = c; batch.push_back(v);
        if (batch.size() >= 3u * 384u) flush();
    };
    if (snap->generated)
    {
        if (ps_r__moc_draw == 2)
        {
            for (const u32 id : snap->hizVisibleAutoClusters)
            {
                if (id >= m_autoClusters.size()) continue;
                const auto& ids = m_autoClusters[id].indices;
                for (size_t k = 0; k + 2 < ids.size(); k += 3)
                    add(m_autoVertices[ids[k]], m_autoVertices[ids[k + 1]], m_autoVertices[ids[k + 2]]);
            }
        }
        else
        {
            for (const auto& cluster : m_autoClusters)
            {
                const auto& ids = cluster.indices;
                for (size_t k = 0; k + 2 < ids.size(); k += 3)
                    add(m_autoVertices[ids[k]], m_autoVertices[ids[k + 1]], m_autoVertices[ids[k + 2]]);
            }
        }
    }
    else if (ps_r__moc_draw == 2)
    {
        for (const u32 id : snap->hizVisibleFaceIds)
        {
            if (id >= m_faces.size()) continue;
            const size_t i = size_t(id) * 3u;
            add(m_vertices[i], m_vertices[i + 1], m_vertices[i + 2]);
        }
    }
    else
    {
        for (size_t i = 0; i + 2 < m_vertices.size(); i += 3)
            add(m_vertices[i], m_vertices[i + 1], m_vertices[i + 2]);
    }
    flush();
#endif
}
