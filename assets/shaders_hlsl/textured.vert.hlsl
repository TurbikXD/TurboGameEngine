cbuffer Constants
{
    float4x4 g_Transform;
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
    float2 TexCoord : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(g_Transform, float4(input.Position, 1.0));
    output.TexCoord = input.TexCoord;
    return output;
}
