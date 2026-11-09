// OGSR R4 temporal GPU Hi-Z. Depth-only static occluders, async readback, TTAPI-safe reads.
#include "stdafx.h"
#include "../xrRender/OGSRHiZ.h"
#include "../../xr_3da/GameFont.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace
{
    template <typename T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

    static const char* kReduceShader = R"HLSL(
Texture2D<float> gInput : register(t0);
RWTexture2D<float> gOutput : register(u0);
cbuffer Params : register(b0) { uint2 inputSize; uint step; uint unused; };
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint2 outputSize;
    gOutput.GetDimensions(outputSize.x, outputSize.y);
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    const uint2 base = id.xy * step;
    float z = 0.0f;
    [loop] for (uint y=0; y<step; ++y)
    [loop] for (uint x=0; x<step; ++x)
    {
        uint2 xy = min(base+uint2(x,y),inputSize-uint2(1,1));
        z = max(z, gInput.Load(int3(xy, 0)));
    }
    gOutput[id.xy] = z;
}
)HLSL";

    struct Level
    {
        unsigned w=0, h=0;
        std::vector<float> depth;
    };
    struct Snapshot
    {
        unsigned frame=0;
        Fmatrix full;
        Fvector camera, dir;
        unsigned coveredPermille=0;
        std::vector<Level> levels;
    };
    struct Pending
    {
        ID3D11Texture2D* staging=nullptr;
        ID3D11Query* fence=nullptr;
        bool pending=false;
        unsigned frame=0;
        Fmatrix full;
        Fvector camera, dir;
    };

    struct Renderer
    {
        ID3D11Device* creator=nullptr; // borrowed identity; do not Release
        ID3D11Texture2D* tex[2]={nullptr,nullptr};
        ID3D11ShaderResourceView* srv[2]={nullptr,nullptr};
        ID3D11UnorderedAccessView* uav[2]={nullptr,nullptr};
        ID3D11ComputeShader* shader=nullptr;
        ID3D11Buffer* constants=nullptr;
        Pending ring[3];
        unsigned fullW=0,fullH=0, w=0,h=0, nextSlot=0;
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        std::atomic<std::shared_ptr<const Snapshot>> published;
#else
        std::shared_ptr<const Snapshot> published;
#endif
        std::atomic<unsigned> tests{0}, rejected{0}, frames{0}, readbacks{0};
        std::atomic<unsigned> skippedCaptures{0}, staleQueries{0}, motionQueries{0};
        std::atomic<unsigned> captureAttempts{0}, badDepth{0}, createFailures{0}, srvFailures{0}, ringBusy{0};
        std::atomic<unsigned> mapFailures{0}, noSnapshot{0}, clearDepth{0};
        std::atomic<unsigned> lastCoveragePermille{0};
        unsigned lastLogFrame=0;
        unsigned lastCaptureFrame=0, lastAdaptiveEvalFrame=0;
        unsigned lastEvaluatedTests=0, lastEvaluatedRejects=0;
        std::atomic<float> rejectionEMA{0.f};
        Fvector lastCaptureCamera, lastCaptureDirection;
        bool hasCaptureCamera=false;
        bool warned=false;

        std::shared_ptr<const Snapshot> Load() const
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            return published.load(std::memory_order_acquire);
#else
            return std::atomic_load_explicit(&published,std::memory_order_acquire);
#endif
        }
        void Publish(std::shared_ptr<const Snapshot> snap)
        {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
            published.store(std::move(snap),std::memory_order_release);
#else
            std::atomic_store_explicit(&published,std::move(snap),std::memory_order_release);
#endif
        }

        void Clear()
        {
            Publish({});
            for (unsigned k=0;k<3;k++) { SafeRelease(ring[k].staging); SafeRelease(ring[k].fence); ring[k].pending=false; }
            for (int k=0;k<2;k++) { SafeRelease(tex[k]); SafeRelease(srv[k]); SafeRelease(uav[k]); }
            SafeRelease(shader); SafeRelease(constants);
            creator=nullptr; fullW=fullH=w=h=nextSlot=0; warned=false;
            lastCaptureFrame=lastAdaptiveEvalFrame=0; lastLogFrame=0;
            lastEvaluatedTests=lastEvaluatedRejects=0;
            rejectionEMA.store(0.f,std::memory_order_relaxed); hasCaptureCamera=false;
        }
        ~Renderer() { Clear(); }
        bool Init(ID3D11Device* device, unsigned srcW, unsigned srcH)
        {
            if (creator==device && fullW==srcW && fullH==srcH && shader && tex[1]) return true;
            Clear();
            if (!device || srcW < 32 || srcH < 32) return false;
            creator=device; fullW=srcW; fullH=srcH;
            const unsigned sizesW[2]={(srcW+1u)/2u,(srcW+3u)/4u};
            const unsigned sizesH[2]={(srcH+1u)/2u,(srcH+3u)/4u};
            w=sizesW[1]; h=sizesH[1];
            ID3DBlob* code=nullptr; ID3DBlob* errors=nullptr;
            const HRESULT compile = D3DCompile(kReduceShader,strlen(kReduceShader),"OGSRHiZ",nullptr,nullptr,
                              "main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
            if (FAILED(compile) || !code)
            {
                Msg("! OGSR HiZ shader compilation failed: %s",errors ? (const char*)errors->GetBufferPointer():"unknown");
                SafeRelease(errors); SafeRelease(code); Clear(); return false;
            }
            SafeRelease(errors);
            const HRESULT shaderHr=device->CreateComputeShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader);
            SafeRelease(code);
            if (FAILED(shaderHr)) { Clear(); return false; }
            D3D11_BUFFER_DESC cb={}; cb.ByteWidth=16; cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            if (FAILED(device->CreateBuffer(&cb,nullptr,&constants))) { Clear(); return false; }
            for (int k=0;k<2;k++)
            {
                D3D11_TEXTURE2D_DESC d={};
                d.Width=sizesW[k]; d.Height=sizesH[k]; d.MipLevels=1; d.ArraySize=1;
                d.Format=DXGI_FORMAT_R32_FLOAT; d.SampleDesc.Count=1; d.Usage=D3D11_USAGE_DEFAULT;
                d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                if (FAILED(device->CreateTexture2D(&d,nullptr,&tex[k])) ||
                    FAILED(device->CreateShaderResourceView(tex[k],nullptr,&srv[k])) ||
                    FAILED(device->CreateUnorderedAccessView(tex[k],nullptr,&uav[k]))) { Clear(); return false; }
            }
            D3D11_TEXTURE2D_DESC stagingDesc={};
            stagingDesc.Width=w; stagingDesc.Height=h; stagingDesc.MipLevels=1; stagingDesc.ArraySize=1;
            stagingDesc.Format=DXGI_FORMAT_R32_FLOAT; stagingDesc.SampleDesc.Count=1;
            stagingDesc.Usage=D3D11_USAGE_STAGING; stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            D3D11_QUERY_DESC qd={}; qd.Query=D3D11_QUERY_EVENT;
            for (auto& p:ring)
            {
                if (FAILED(device->CreateTexture2D(&stagingDesc,nullptr,&p.staging)) ||
                    FAILED(device->CreateQuery(&qd,&p.fence))) { Clear(); return false; }
            }
            Msg("* OGSR HiZ: DX11 temporal pyramid %ux%u (source %ux%u), async readback",w,h,srcW,srcH);
            return true;
        }
        void Poll(ID3D11DeviceContext* ctx)
        {
            for (auto& p:ring)
            {
                if (!p.pending || ctx->GetData(p.fence,nullptr,0,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK) continue;
                D3D11_MAPPED_SUBRESOURCE map={};
                const HRESULT mapped=ctx->Map(p.staging,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&map);
                if (mapped!=S_OK) { if (mapped!=DXGI_ERROR_WAS_STILL_DRAWING) mapFailures.fetch_add(1,std::memory_order_relaxed); continue; }
                auto next=std::make_shared<Snapshot>();
                next->frame=p.frame; next->camera=p.camera; next->dir=p.dir; next->full=p.full;
                next->levels.emplace_back();
                Level& root=next->levels.back(); root.w=w; root.h=h; root.depth.resize(size_t(w)*h);
                unsigned covered=0;
                for (unsigned y=0;y<h;y++)
                {
                    const float* src=reinterpret_cast<const float*>(static_cast<const unsigned char*>(map.pData)+size_t(y)*map.RowPitch);
                    std::copy(src,src+w,root.depth.begin()+size_t(y)*w);
                    for (unsigned x=0; x<w; ++x)
                        if (std::isfinite(src[x]) && src[x]>=0.f && src[x]<0.9999f) ++covered;
                }
                const unsigned permille = static_cast<unsigned>(1000ull * covered / (size_t(w)*h));
                next->coveredPermille=permille;
                lastCoveragePermille.store(permille,std::memory_order_relaxed);
                if (covered==0) clearDepth.fetch_add(1,std::memory_order_relaxed);
                ctx->Unmap(p.staging,0); p.pending=false;
                while (next->levels.back().w>1 || next->levels.back().h>1)
                {
                    const Level& old=next->levels.back();
                    const unsigned nw=(old.w+1u)/2u, nh=(old.h+1u)/2u;
                    Level lev; lev.w=nw; lev.h=nh; lev.depth.resize(size_t(nw)*nh);
                    for (unsigned y=0;y<nh;y++) for(unsigned x=0;x<nw;x++)
                    {
                        float maxDepth=0.f;
                        for(unsigned dy=0;dy<2;dy++) for(unsigned dx=0;dx<2;dx++)
                        {
                            unsigned sx=std::min(old.w-1,x*2+dx),sy=std::min(old.h-1,y*2+dy);
                            maxDepth=std::max(maxDepth,old.depth[size_t(sy)*old.w+sx]);
                        }
                        lev.depth[size_t(y)*nw+x]=maxDepth;
                    }
                    next->levels.push_back(std::move(lev));
                }
                auto current=Load();
                if (!current || next->frame>current->frame) Publish(std::move(next));
                readbacks.fetch_add(1,std::memory_order_relaxed);
            }
        }
        void Capture(ID3D11Device* device,ID3D11DeviceContext* ctx,ID3D11DepthStencilView* dsv,
                     const Fmatrix& full,const Fvector& cam,const Fvector& dir,unsigned frame)
        {
            if (ps_r__hiz<=0) return;
            captureAttempts.fetch_add(1,std::memory_order_relaxed);
            if (!ctx || !dsv || !device) { badDepth.fetch_add(1,std::memory_order_relaxed); return; }
            ID3D11Resource* res=nullptr; dsv->GetResource(&res);
            if (!res) { badDepth.fetch_add(1,std::memory_order_relaxed); return; }
            ID3D11Texture2D* depth=nullptr;
            if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D),(void**)&depth))) { badDepth.fetch_add(1,std::memory_order_relaxed); SafeRelease(res); return; }
            SafeRelease(res);
            D3D11_TEXTURE2D_DESC d={}; depth->GetDesc(&d);
            if (d.SampleDesc.Count!=1 || d.MipLevels!=1 ||
                (d.Format!=DXGI_FORMAT_R24G8_TYPELESS && d.Format!=DXGI_FORMAT_R32_TYPELESS) ||
                !(d.BindFlags&D3D11_BIND_SHADER_RESOURCE))
            {
                if (!warned) { Msg("! OGSR HiZ: depth format/MSAA/SRV unsupported; fail open"); warned=true; }
                badDepth.fetch_add(1,std::memory_order_relaxed);
                SafeRelease(depth); return;
            }
            if (!Init(device,d.Width,d.Height)) { createFailures.fetch_add(1,std::memory_order_relaxed); SafeRelease(depth); return; }
            // Deferred contexts cannot return query results or map a staging
            // texture for CPU reads. Poll on OGSR's immediate D3D11 context,
            // even when this frame's capture commands are recorded elsewhere.
            Poll(HW.pContext);

            // Adaptive mode 2 keeps the existing TTAPI query path active, but
            // amortizes GPU captures when the camera is practically stationary.
            // Moving/turning always requests a fresh capture. Readback is still
            // asynchronous; never wait for the GPU here.
            if (ps_r__hiz==2)
            {
                if (frame-lastAdaptiveEvalFrame>=30u)
                {
                    const unsigned nt=tests.load(std::memory_order_relaxed);
                    const unsigned nr=rejected.load(std::memory_order_relaxed);
                    const unsigned dt=nt-lastEvaluatedTests;
                    const unsigned dr=nr-lastEvaluatedRejects;
                    if (dt>=8u)
                    {
                        const float eff=std::min(1.f,float(dr)/float(dt));
                        const float oldEMA=rejectionEMA.load(std::memory_order_relaxed);
                        rejectionEMA.store(lastAdaptiveEvalFrame==0u ? eff : oldEMA*0.8f+eff*0.2f,
                                           std::memory_order_relaxed);
                    }
                    lastEvaluatedTests=nt; lastEvaluatedRejects=nr;
                    lastAdaptiveEvalFrame=frame;
                }
                if (hasCaptureCamera && frame>lastCaptureFrame)
                {
                    const float move2=cam.distance_to_sqr(lastCaptureCamera);
                    const float turn=dir.dotproduct(lastCaptureDirection);
                    const bool moved=move2>0.02f*0.02f || turn<0.99995f;
                    const unsigned interval=moved ? 1u : (rejectionEMA.load(std::memory_order_relaxed)>=0.10f ? 2u : 3u);
                    if (frame-lastCaptureFrame<interval)
                    {
                        skippedCaptures.fetch_add(1,std::memory_order_relaxed);
                        return;
                    }
                }
            }
            Pending& p=ring[nextSlot];
            if (p.pending) { ringBusy.fetch_add(1,std::memory_order_relaxed); SafeRelease(depth); return; } // never block the GPU
            ID3D11ShaderResourceView* input=nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd={}; sd.Format=(d.Format==DXGI_FORMAT_R24G8_TYPELESS)
                ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : DXGI_FORMAT_R32_FLOAT;
            sd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels=1;
            if (FAILED(device->CreateShaderResourceView(depth,&sd,&input))) { srvFailures.fetch_add(1,std::memory_order_relaxed); SafeRelease(depth); return; }
            SafeRelease(depth);
            ctx->CSSetShader(shader,nullptr,0);
            ctx->CSSetConstantBuffers(0,1,&constants);
            for (unsigned k=0;k<2;k++)
            {
                const unsigned dims[4]={k==0?d.Width:(d.Width+1u)/2u,
                                          k==0?d.Height:(d.Height+1u)/2u,2u,0u};
                ctx->UpdateSubresource(constants,0,nullptr,dims,0,0);
                ID3D11ShaderResourceView* from=k==0 ? input : srv[0];
                ctx->CSSetShaderResources(0,1,&from);
                ctx->CSSetUnorderedAccessViews(0,1,&uav[k],nullptr);
                D3D11_TEXTURE2D_DESC td={}; tex[k]->GetDesc(&td);
                ctx->Dispatch((td.Width+7u)/8u,(td.Height+7u)/8u,1);
                ID3D11UnorderedAccessView* nullUav=nullptr;
                ID3D11ShaderResourceView* nullSrv=nullptr;
                ctx->CSSetUnorderedAccessViews(0,1,&nullUav,nullptr);
                ctx->CSSetShaderResources(0,1,&nullSrv);
            }
            SafeRelease(input);
            ID3D11Buffer* nullBuffer=nullptr;
            ctx->CSSetConstantBuffers(0,1,&nullBuffer);
            ctx->CSSetShader(nullptr,nullptr,0);
            ctx->CopyResource(p.staging,tex[1]); ctx->End(p.fence);
            p.full=full; p.camera=cam; p.dir=dir; p.frame=frame; p.pending=true;
            lastCaptureFrame=frame; lastCaptureCamera=cam; lastCaptureDirection=dir;
            hasCaptureCamera=true;
            nextSlot=(nextSlot+1u)%3u;
            frames.fetch_add(1,std::memory_order_relaxed);
        }
        bool Ready() const
        {
            if (ps_r__hiz<=0) return false;
            const auto snap=Load();
            return snap && !snap->levels.empty() && Device.dwFrame>snap->frame &&
                Device.dwFrame-snap->frame<=4u && snap->coveredPermille>0u;
        }
        bool Occluded(const Fbox& b, bool countStats=true,
                      const std::shared_ptr<const Snapshot>& pinned={},
                      const Fmatrix* pinnedFull=nullptr, const Fvector* pinnedCamera=nullptr,
                      const Fvector* pinnedDirection=nullptr, unsigned pinnedFrame=0,
                      bool forMocSelection=false)
        {
            // Mode 1: Hi-Z may rank/filter CPU MOC occluders, but it must
            // NEVER cull actual scene objects on its own. Mode 2 does both.
            if (ps_r__hiz<=0 || (ps_r__hiz==1 && !forMocSelection)) return false;
            const auto snap=pinned ? pinned : Load();
            if (!snap || snap->levels.empty() || snap->coveredPermille==0)
            { if(countStats) noSnapshot.fetch_add(1,std::memory_order_relaxed); return false; }
            const unsigned now=pinned ? pinnedFrame : Device.dwFrame;
            const Fvector& currentCamera=pinnedCamera ? *pinnedCamera : Device.vCameraPosition;
            const Fvector& currentDirection=pinnedDirection ? *pinnedDirection : Device.vCameraDirection;
            const Fmatrix& currentFull=pinnedFull ? *pinnedFull : Device.mFullTransform;
            if (now<=snap->frame || now-snap->frame>4u)
            {
                if(countStats) staleQueries.fetch_add(1,std::memory_order_relaxed);
                return false; // stale or incomplete readback: fail open
            }

            // Oblivion temporal Hi-Z: allow ordinary camera motion, not camera
            // cuts/teleports. Reproject the *same static box* with both matrices
            // and test the union footprint in the historical depth buffer.
            const float move2=currentCamera.distance_to_sqr(snap->camera);
            const float dot=currentDirection.dotproduct(snap->dir);
            if (move2>0.75f*0.75f || dot<0.9961947f) // 0.75 m or ~5 degrees
            {
                if(countStats) motionQueries.fetch_add(1,std::memory_order_relaxed);
                return false;
            }
            float xmin=1.f,ymin=1.f,xmax=-1.f,ymax=-1.f,nearest=1.f;
            const Fmatrix& oldM=snap->full;
            const Fmatrix& newM=currentFull;
            for (unsigned i=0;i<8;i++)
            {
                const float x=(i&1)?b.max.x:b.min.x;
                const float y=(i&2)?b.max.y:b.min.y;
                const float z=(i&4)?b.max.z:b.min.z;
                const float oldZ=x*oldM._13+y*oldM._23+z*oldM._33+oldM._43;
                const float oldW=x*oldM._14+y*oldM._24+z*oldM._34+oldM._44;
                const float newZ=x*newM._13+y*newM._23+z*newM._33+newM._43;
                const float newW=x*newM._14+y*newM._24+z*newM._34+newM._44;
                if (oldW<=0.05f || newW<=0.05f || oldZ<=0.f || newZ<=0.f)
                    return false;
                const float oldX=(x*oldM._11+y*oldM._21+z*oldM._31+oldM._41)/oldW;
                const float oldY=(x*oldM._12+y*oldM._22+z*oldM._32+oldM._42)/oldW;
                const float newX=(x*newM._11+y*newM._21+z*newM._31+newM._41)/newW;
                const float newY=(x*newM._12+y*newM._22+z*newM._32+newM._42)/newW;
                const float oldD=oldZ/oldW;
                if (!std::isfinite(oldX)||!std::isfinite(oldY)||!std::isfinite(newX)||
                    !std::isfinite(newY)||!std::isfinite(oldD)) return false;
                xmin=std::min(xmin,std::min(oldX,newX));
                xmax=std::max(xmax,std::max(oldX,newX));
                ymin=std::min(ymin,std::min(oldY,newY));
                ymax=std::max(ymax,std::max(oldY,newY));
                nearest=std::min(nearest,oldD);
            }
            // Test the *visible* screen portion of partly off-screen boxes.
            // Still fail open for the near plane, where 8-corner projection isn't valid.
            if (xmax<=-1.f || ymax<=-1.f || xmin>=1.f || ymin>=1.f ||
                nearest<=0.f || nearest>=1.f)
                return false;
            const Level& first=snap->levels.front();
            if (!first.w||!first.h||first.depth.size()<size_t(first.w)*first.h) return false;

            const unsigned age=now-snap->frame;
            // Motion-dependent dilation, instead of the old unconditional 3
            // HZB pixels. This keeps stationary distant occluders effective.
            unsigned motionPad=0;
            if (age>1u || move2>0.08f*0.08f || dot<0.99985f) motionPad=1;
            if (age>2u || move2>0.30f*0.30f || dot<0.99860f) motionPad=2;
            const float pad=float(motionPad);
            int x0=static_cast<int>(std::floor((xmin+1.f)*0.5f*first.w-pad));
            int x1=static_cast<int>(std::ceil((xmax+1.f)*0.5f*first.w+pad));
            int y0=static_cast<int>(std::floor((1.f-ymax)*0.5f*first.h-pad));
            int y1=static_cast<int>(std::ceil((1.f-ymin)*0.5f*first.h+pad));
            if (x1<0 || y1<0 || x0>=int(first.w) || y0>=int(first.h) || x0>x1 || y0>y1)
                return false;
            x0=std::max(0,x0); y0=std::max(0,y0);
            x1=std::min(int(first.w)-1,x1); y1=std::min(int(first.h)-1,y1);
            unsigned mip=0;
            const int span=(nearest>0.997f) ? 7 : 3;
            while (size_t(mip)+1u<snap->levels.size() && (x1-x0>span||y1-y0>span))
            { ++mip; x0/=2;y0/=2;x1/=2;y1/=2; }
            const Level& l=snap->levels[mip];
            if (!l.w||!l.h||l.depth.size()<size_t(l.w)*l.h ||
                x0<0||y0<0||x1>=int(l.w)||y1>=int(l.h)) return false;
            float maximum=0.f;
            for (int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++)
            {
                const float depth=l.depth[size_t(y)*l.w+x];
                if (!std::isfinite(depth)||depth<0.f||depth>1.f) return false;
                maximum=std::max(maximum,depth);
            }
            if(countStats) tests.fetch_add(1,std::memory_order_relaxed);
            // Normalised D3D depth, not the linear eye-space depth in
            // Oblivion. Use a small bias which increases with snapshot age.
            const float bias=0.00005f+float(age-1u)*0.00002f;
            const bool hidden=nearest>maximum+bias;
            if (hidden && ps_r__hiz==2 && countStats) rejected.fetch_add(1,std::memory_order_relaxed);
            // Object queries cannot reach this point in mode 1; MOC-only
            // queries explicitly opt in with forMocSelection=true.
            return hidden;
        }
    };
    Renderer& Get() { static Renderer instance; return instance; }
}

namespace OGSRHiZ
{
    OccluderSelection BeginOccluderSelection(const Fmatrix& full, const Fvector& camera,
                                             const Fvector& direction, unsigned frame)
    {
        OccluderSelection selection;
        // Both modes 1 and 2 use GPU Hi-Z to choose useful MOC occluders.
        // Only mode 2 additionally culls scene objects in CHOM::visible().
        if (ps_r__hiz<=0) return selection;
        auto snap=Get().Load();
        if (!snap || snap->levels.empty() || snap->coveredPermille==0 ||
            frame<=snap->frame || frame-snap->frame>4u)
            return selection;
        // A camera cut must never use a previous-frame occlusion mask.
        if (camera.distance_to_sqr(snap->camera)>0.75f*0.75f ||
            direction.dotproduct(snap->dir)<0.9961947f)
            return selection;
        selection.snapshot=std::static_pointer_cast<const void>(snap);
        selection.full=full;
        selection.camera=camera;
        selection.direction=direction;
        selection.frame=frame;
        selection.enabled=true;
        return selection;
    }
    bool OccludedForOccluder(const OccluderSelection& selection, const Fbox& box)
    {
        if (!selection.enabled || !selection.snapshot || ps_r__hiz<=0) return false;
        const auto snap=std::static_pointer_cast<const Snapshot>(selection.snapshot);
        // A failed projection is NEVER hidden. Do not contribute to object query
        // counters: these are the MOC build's independent cluster probes.
        return Get().Occluded(box,false,snap,&selection.full,&selection.camera,
                              &selection.direction,selection.frame,true);
    }
    void Capture(ID3D11Device* d,ID3D11DeviceContext* c,ID3D11DepthStencilView* depth,
                 const Fmatrix& full,const Fvector& cam,const Fvector& dir,unsigned frame)
    { Get().Capture(d,c,depth,full,cam,dir,frame); }
    void PumpReadback(ID3D11DeviceContext* immediate)
    {
        if (ps_r__hiz<=0 || !immediate) return;
        Renderer& r=Get();
        r.Poll(immediate);
        // Normal operation is silent. GPU/readback counters remain available
        // through OGSRHiZ::Stats() in the on-screen performance statistics.
        // Keep one-shot warnings and shader compilation errors in the log.
    }
    bool Occluded(const Fbox& box) { return Get().Occluded(box); }
    bool SnapshotReady() { return Get().Ready(); }
    bool OccludedDebug(const Fbox& box) { return ps_r__hiz==2 && Get().Occluded(box,false); }
    void Reset() { Get().Clear(); }
    void Stats(CGameFont& font)
    {
        Renderer& r=Get();
        auto snap=r.Load();
        font.OutNext("OGSR HiZ: mode %d snap %u gpu %u readback %u skipped %u",
            ps_r__hiz,snap?snap->frame:0u,r.frames.load(),r.readbacks.load(),r.skippedCaptures.load());
        font.OutNext("  queries %u culled %u efficiency %.1f%% stale %u motion %u",
            r.tests.load(),r.rejected.load(),r.rejectionEMA.load(std::memory_order_relaxed)*100.f,
            r.staleQueries.load(),r.motionQueries.load());
        font.OutNext("  depth coverage %.1f%%, no snapshot %u, invalid DSV %u, busy %u",
            float(r.lastCoveragePermille.load())*0.1f,r.noSnapshot.load(),r.badDepth.load(),r.ringBusy.load());
    }
}
