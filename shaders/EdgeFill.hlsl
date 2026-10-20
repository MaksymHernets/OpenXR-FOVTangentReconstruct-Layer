cbuffer Settings : register(b0) {
    float2 fovScale; float2 fovBias;
    float4 sourceRect;
    float2 texel; uint sourceSlice; uint method;
    float stretchWidth; float gradientStrength; float blurRadius; float blurStrength;
    uint edgeSamples; float3 padding;
    float4 fillColor;
};
Texture2DArray<float4> sourceImage : register(t0);
SamplerState linearClamp : register(s0);
struct Vertex { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Vertex VS(uint id : SV_VertexID) {
    Vertex v; v.uv=float2((id<<1)&2,id&2);v.pos=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;
}
float4 sampleImage(float2 uv) {
    float2 lo=sourceRect.xy+0.5*texel, hi=sourceRect.xy+sourceRect.zw-0.5*texel;
    return sourceImage.SampleLevel(linearClamp,float3(clamp(sourceRect.xy+uv*sourceRect.zw,lo,hi),sourceSlice),0);
}
float boundedStretch(float x) {
    float w=max(stretchWidth,0.01);
    float z=min(x,1-x);
    float mapped=z<w?lerp(saturate(z),w*exp(clamp((z-w)/w,-40,0)),gradientStrength):z;
    return x<0.5?mapped:1-mapped;
}
// TEMPORAL_IMPL
float4 PS(Vertex v) : SV_Target {
    float2 q=v.uv*fovScale+fovBias;
    float2 outside=max(max(-q,q-1),0);
    float distance=max(outside.x,outside.y);
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
