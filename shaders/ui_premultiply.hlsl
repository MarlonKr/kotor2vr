// UI is captured from KOTOR separately and composed after neural world
// processing, normally through an OpenXR quad layer. This conversion makes the
// alpha convention explicit before a texture is handed to the runtime.

Texture2D<float4> UiColor : register(t0);
SamplerState LinearClamp : register(s0);

cbuffer UiConstants : register(b0) {
    float2 UvScale;
    float2 UvOffset;
    float Opacity;
    float3 Padding;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput VsMain(uint vertex_id : SV_VertexID) {
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    const float2 uvs[3] = {
        float2(0.0, 1.0),
        float2(0.0, -1.0),
        float2(2.0, 1.0),
    };

    VertexOutput output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = uvs[vertex_id] * UvScale + UvOffset;
    return output;
}

float4 PsMain(VertexOutput input) : SV_Target0 {
    float4 color = UiColor.SampleLevel(LinearClamp, input.uv, 0.0);
    color.a = saturate(color.a * Opacity);
    color.rgb *= color.a;
    return color;
}

