cbuffer Settings : register(b0) {
    float2 fovScale; float2 fovBias;
    float4 sourceRect;
    float2 texel; uint sourceSlice; uint method;
    float stretchWidth; float gradientStrength; float blurRadius; float blurStrength;
    uint edgeSamples; float3 padding;
    float4 fillColor;
    float4 outsideMask; float4 outsidePolicy; float4 centerMask;
};
Texture2DArray<float4> sourceImage : register(t0);
SamplerState linearClamp : register(s0);
struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v; v.uv=float2((id<<1)&2,id&2);v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;
}
float maskRadius(float2 uv, float2 radii, float shape) {
    float2 d=abs(uv-outsideMask.xy)/max(radii,float2(.0001,.0001));
    return shape>1.5 ? max(d.x,d.y) : length(d);
}
float2 validObservation(float2 uv) {
    if(outsidePolicy.x>0) {
        float r=maskRadius(uv,outsideMask.zw,outsidePolicy.x);
        // Half-texel guard prevents bilinear filtering across the missing boundary.
        float guard=max(texel.x/sourceRect.z,texel.y/sourceRect.w)/min(outsideMask.z,outsideMask.w);
        uv=outsideMask.xy+(uv-outsideMask.xy)*min(1,max(.5,1-guard)/max(r,.0001));
    }
    return uv;
}
float4 originalSample(float2 uv) {
    float2 lo=sourceRect.xy+0.5*texel, hi=sourceRect.xy+sourceRect.zw-0.5*texel;
    return sourceImage.SampleLevel(linearClamp,float3(clamp(sourceRect.xy+uv*sourceRect.zw,lo,hi),sourceSlice),0);
}
float4 sampleImage(float2 uv) { return originalSample(validObservation(uv)); }
float boundedStretch(float x) {
    float w=max(stretchWidth,0.01);
    float z=min(x,1-x);
    float mapped=z<w?lerp(saturate(z),w*exp(clamp((z-w)/w,-40,0)),gradientStrength):z;
    return x<0.5?mapped:1-mapped;
}
// TEMPORAL_IMPL
float4 reconstruct(Vertex v) {
    float2 q=v.uv*fovScale+fovBias;
    float2 outside=max(max(-q,q-1),0);
    float distance=max(outside.x,outside.y);
    if(outsidePolicy.x>0) {
        float r=maskRadius(q,outsideMask.zw,outsidePolicy.x);
        distance=max(0,r-1)*min(outsideMask.z,outsideMask.w);
        // The observed Center/Middle must remain byte-for-byte unchanged.
        if(distance<=0)return originalSample(q);
        if(method==3)q=outsideMask.xy+(q-outsideMask.xy)*(max(0,2-r)/max(r,.0001));
    }
    if(outsidePolicy.w>0&&distance<=0)return originalSample(q);
    if(method==5)return temporalPixel(v,q,distance);
    if(padding.x>0 && all(q>-0.01) && all(q<1.01) &&
       min(min(abs(q.x),abs(q.x-1)),min(abs(q.y),abs(q.y-1)))<0.004) return float4(0,1,1,1);
    if(method==4) return distance>0 ? fillColor : sampleImage(q);
    if(method==1) {
        float2 p=float2(boundedStretch(q.x),boundedStretch(q.y));
        float4 sum=0;uint count=clamp(edgeSamples,1u,16u);
        // Bounded inward samples; converge to a single sample in the valid interior.
        float amount=smoothstep(0,stretchWidth,distance)*stretchWidth*0.15*gradientStrength;
        for(uint i=0;i<count;i++){float t=count>1?(float)i/(count-1):0;sum+=sampleImage(lerp(p,float2(0.5,0.5),t*amount));}
        return sum/count;
    }
    if(method==3){float2 mirror=1-abs(1-2*frac(q*0.5));return sampleImage(mirror);}
    float4 center=sampleImage(saturate(q));
    if(method==2&&distance>0) {
        float4 blurred=0;
        // Nine fixed taps cap sampling cost independently of the radius.
        for(int y=-1;y<=1;y++) for(int x=-1;x<=1;x++)
            blurred+=sampleImage(saturate(q)+float2(x,y)*texel/sourceRect.zw*blurRadius);
        return lerp(center,blurred/9,blurStrength*smoothstep(0,stretchWidth,distance));
    }
    return center;
}

float4 PS(Vertex v) : SV_Target {
    float2 q=v.uv*fovScale+fovBias;
    float2 cropped=max(max(-q,q-1),0);
    bool crop=max(cropped.x,cropped.y)>0;
    bool outside=crop||(outsidePolicy.x>0&&maskRadius(q,outsideMask.zw,outsidePolicy.x)>1);
    uint debug=(uint)outsidePolicy.y;
    if(debug==5)return originalSample(v.uv); // raw reconstruction input
    if(debug>=1&&debug<=4) {
        bool center=maskRadius(q,centerMask.xy+centerMask.zz,centerMask.w)<=1;
        bool selected=debug==1?(!outside&&center):debug==2?(!outside&&!center):debug==3?outside:crop;
        return selected ? float4(debug==1?float3(0,1,.5):debug==2?float3(1,.6,0):float3(1,0,1),1):float4(0,0,0,1);
    }
    if(debug==7&&outside)return fillColor;
    float4 color=reconstruct(v);
    if(!all(isfinite(color))) {
        float d=max(max(cropped.x,cropped.y),.001);
        color=outsidePolicy.z>0?fillColor:adaptiveBlur(q,d);
        if(!all(isfinite(color)))color=fillColor;
    }
    if(debug==6&&!outside)return float4(0,0,0,1);
    return color;
}
