cbuffer Constants
{
    float4x4 g_ModelViewProjection;
    float4x4 g_Model;
    float4 g_UvScaleBias;
};

struct VSInput
{
    float3 Position : ATTRIB0;
    float3 Normal : ATTRIB1;
    float2 TexCoord : ATTRIB2;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float3 WorldNormal : TEXCOORD0;
    float3 WorldPosition : TEXCOORD1;
    float2 TexCoord : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    const float4 worldPosition = mul(g_Model, float4(input.Position, 1.0));
    output.Position = mul(g_ModelViewProjection, float4(input.Position, 1.0));
    output.WorldPosition = worldPosition.xyz;
    output.WorldNormal = normalize(mul((float3x3)g_Model, input.Normal));
    output.TexCoord = input.TexCoord * g_UvScaleBias.xy + g_UvScaleBias.zw;
    return output;
}
