cbuffer Constants
{
    float4x4 g_Transform;
};

struct VSInput
{
    float2 Position : ATTRIB0;
    float3 Color : ATTRIB1;
};

struct VSOutput
{
    float4 Position : SV_POSITION;
    float3 Color : COLOR0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.Position = mul(g_Transform, float4(input.Position, 0.0, 1.0));
    output.Color = input.Color;
    return output;
}
