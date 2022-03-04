cbuffer ConstantBuffer: register(b0)
{
    float4x4 mwpMatrix;
}
Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

PSInput VSMain(
    float4 position : POSITION,
    float4 normal: NORMAL,
    float4 ambient : COLOR0,
    float4 diffuse : COLOR1,
    float4 emissive : COLOR2,
    float4 texcoords: TEXCOORD)
{
    PSInput result;


    result.position = mul(mwpMatrix, position);
    result.color = diffuse;
    result.uv = texcoords.xy;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}

float4 PSMain_texture(PSInput input) : SV_TARGET
{
    return g_texture.Sample(g_sampler, input.uv);
}