// Only original observations are bound here: reconstructed output is never history.
struct HistoryData {float4 rotation;float4 translationAge;float4 tangents;float4 depth;float4 dimensions;};
cbuffer TemporalSettings : register(b1) {
    float4 outputTangents;float4 temporalSettings;float4 adaptiveFallback;float4 temporalDebug;float4 outputSize;
    HistoryData frames[8];
};
Texture2DArray<float4> historyColor[8] : register(t1);
Texture2DArray<float> historyDepth[8] : register(t9);
RWStructuredBuffer<uint> coverage : register(u1);
// Shader Model 5.0 requires literal resource indices (D3D11 compatibility).
float loadDepth(uint i,int2 p){switch(i){case 0:return historyDepth[0].Load(int4(p,0,0));case 1:return historyDepth[1].Load(int4(p,0,0));case 2:return historyDepth[2].Load(int4(p,0,0));case 3:return historyDepth[3].Load(int4(p,0,0));case 4:return historyDepth[4].Load(int4(p,0,0));case 5:return historyDepth[5].Load(int4(p,0,0));case 6:return historyDepth[6].Load(int4(p,0,0));case 7:return historyDepth[7].Load(int4(p,0,0));}return -1;}
float4 historySample(uint i,float2 uv){switch(i){case 0:return historyColor[0].SampleLevel(linearClamp,float3(uv,0),0);case 1:return historyColor[1].SampleLevel(linearClamp,float3(uv,0),0);case 2:return historyColor[2].SampleLevel(linearClamp,float3(uv,0),0);case 3:return historyColor[3].SampleLevel(linearClamp,float3(uv,0),0);case 4:return historyColor[4].SampleLevel(linearClamp,float3(uv,0),0);case 5:return historyColor[5].SampleLevel(linearClamp,float3(uv,0),0);case 6:return historyColor[6].SampleLevel(linearClamp,float3(uv,0),0);case 7:return historyColor[7].SampleLevel(linearClamp,float3(uv,0),0);}return 0;}
float3 qrotate(float4 q,float3 v){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
float2 projectRay(float3 p,float4 f){return float2((-p.x/p.z-f.x)/(f.y-f.x),(f.z+p.y/p.z)/(f.z-f.w));}
float3 viewRay(float2 uv,float4 f){return float3(lerp(f.x,f.y,uv.x),lerp(f.z,f.w,uv.y),-1);}
bool inImage(float2 uv){return all(isfinite(uv))&&all(uv>=0)&&all(uv<=1);}
float metricDepth(uint index,float2 uv){
    HistoryData h=frames[index];if(!inImage(uv))return -1;
    int2 size=int2(h.dimensions.xy);int2 p=clamp(int2(uv*size),0,size-1);
    float value=loadDepth(index,p);
    if(!isfinite(value)||value<h.depth.x||value>h.depth.y)return -1;
    float t=(value-h.depth.x)/(h.depth.y-h.depth.x);
    // Clear/infinite endpoints contain no reliable surface observation.
    if(t<=1e-6||t>=1-1e-6)return -1;
    float inv=lerp(h.depth.z,h.depth.w,t);return inv>1e-6?rcp(inv):-1;
}
float4 adaptiveBlur(float2 q,float distance){
    float2 edge=saturate(q);float blend=smoothstep(0,max(adaptiveFallback.z,.001),distance);
    float radius=adaptiveFallback.x*lerp(.35,1,blend);float4 sum=0;
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)sum+=sampleImage(edge+float2(x,y)*texel/sourceRect.zw*radius);
    return lerp(sampleImage(edge),sum/9,adaptiveFallback.y*blend);
}
float4 temporalPixel(Vertex v,float2 q,float outsideDistance){
    if(outsideDistance<=0)return sampleImage(q);
    float4 fallback=adaptiveBlur(q,outsideDistance);fallback.a=1;float3 ray=viewRay(v.uv,outputTangents);
    float4 chosen=0;float confidence=0,chosenDepth=1e20;bool usedDepth=false;uint consistent=0;
    [loop]for(uint i=0;i<min(8u,(uint)temporalSettings.x);i++){
        HistoryData h=frames[i];float4 invQ=float4(-h.rotation.xyz,h.rotation.w);
        float3 hray=qrotate(invQ,ray);if(hray.z>=-1e-5)continue;
        float2 uv=projectRay(hray,h.tangents);bool depth=h.dimensions.z>0;float distance=-1,error=0;
        if(depth){
            if(any(uv<-.08)||any(uv>1.08))continue;uv=saturate(uv);
            [loop]for(int iteration=0;iteration<3;iteration++){
                float z=metricDepth(i,uv);if(z<=0)break;
                float3 pc=qrotate(h.rotation,viewRay(uv,h.tangents)*z)+h.translationAge.xyz;
                if(pc.z>=-1e-4)break;
                uv=projectRay(qrotate(invQ,ray*(-pc.z)-h.translationAge.xyz),h.tangents);
            }
            float z=metricDepth(i,uv);if(z<=0)continue;
            float3 pc=qrotate(h.rotation,viewRay(uv,h.tangents)*z)+h.translationAge.xyz;
            if(pc.z>=-1e-4)continue;distance=-pc.z;
            error=length((projectRay(pc,outputTangents)-v.uv)*outputSize.xy);
            if(!isfinite(error)||error>1.5)continue;
            // Reject silhouettes and steep, contradictory depth neighborhoods.
            float zx=metricDepth(i,uv+float2(rcp(max(h.dimensions.x,1)),0));
            float zy=metricDepth(i,uv+float2(0,rcp(max(h.dimensions.y,1))));
            if(zx<=0||zy<=0||max(abs(zx-z),abs(zy-z))>max(.03,z*.08))continue;
        }else if(!inImage(uv))continue;
        float angle=2*acos(saturate(abs(h.rotation.w)));
        float translation=length(h.translationAge.xyz);
        float c=(depth?.98:.82)*exp(-h.translationAge.w/max(h.dimensions.w,.02))*exp(-translation/(depth?.6:.025));
        c*=saturate(1-angle/2)*saturate(1-error/2);
        c*=exp(-outsideDistance/2);
        float4 color=historySample(i,uv);
        if(!all(isfinite(color)))continue;color.a=1;
        if(confidence>0&&usedDepth&&depth){
            float tolerance=max(.04,min(distance,chosenDepth)*.05);
            if(distance>chosenDepth+tolerance)continue;
            if(distance<chosenDepth-tolerance){chosen=color;confidence=c;chosenDepth=distance;consistent=0;continue;}
        }
        // Blend only compatible surfaces/colors; prefer the newest sample on disagreement.
        float colorDifference=length(color.rgb-chosen.rgb)/max(1,length(chosen.rgb));
        if(confidence>0&&usedDepth==depth&&colorDifference<.15){
            chosen=lerp(chosen,color,c/(confidence+c));consistent++;confidence=max(confidence,c);
        }else if(confidence<=0||c>confidence*1.2){chosen=color;confidence=c;usedDepth=depth;chosenDepth=depth?distance:1e20;}
    }
    confidence=saturate(confidence*(1+min(consistent,2u)*.04));
    float weight=smoothstep(temporalSettings.y,min(1,temporalSettings.y+adaptiveFallback.w),confidence);
    weight*=temporalSettings.z*smoothstep(0,max(temporalSettings.w,.001),outsideDistance);
    // Sparse GPU counters are diagnostics only; color/depth textures never leave the GPU.
    if(temporalDebug.w>0&&(((uint)v.pos.x&7)==0)&&(((uint)v.pos.y&7)==0)){
        InterlockedAdd(coverage[0],1);if(weight>.001)InterlockedAdd(coverage[1],1);
        if(weight>.001&&usedDepth)InterlockedAdd(coverage[2],1);InterlockedAdd(coverage[3],(uint)round(weight*255));
    }
    if(temporalDebug.x>0)return float4(confidence.xxx,1);
    if(temporalDebug.y>0)return float4(weight,1-weight,0,1);
    if(temporalDebug.z>0)return usedDepth&&weight>0?float4((1/(1+chosenDepth)).xxx,1):float4(1,0,1,1);
    return lerp(fallback,chosen,weight);
}
