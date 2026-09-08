// Spatial copy used after per-eye world processing. This shader is deliberately
// temporal-state free: left and right histories are owned by the DLSS backend,
// never by this compositor.

Texture2D<float4> SourceColor : register(t0);
SamplerState LinearClamp : register(s0);

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
    output.uv = uvs[vertex_id];
    return output;
}

float4 PsMain(VertexOutput input) : SV_Target0 {
    return SourceColor.SampleLevel(LinearClamp, input.uv, 0.0);
}

