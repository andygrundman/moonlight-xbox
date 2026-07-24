#ifndef SPIRV_CROSS_CONSTANT_ID_0
#define SPIRV_CROSS_CONSTANT_ID_0 false
#endif
static const bool DCShift = SPIRV_CROSS_CONSTANT_ID_0;
static const uint3 gl_WorkGroupSize = uint3(64u, 1u, 1u);

cbuffer Registers
{
    int2 _150_resolution : packoffset(c0);
    float2 _150_inv_resolution : packoffset(c0.z);
};

Texture2DArray<float4> uTexture : register(t0);
SamplerState _uTexture_sampler : register(s0);
RWTexture2DArray<float4> uOutput : register(u1);

static uint3 gl_WorkGroupID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint shared_block[20][41];
static uint local_index;

uint spvPackHalf2x16(float2 value)
{
    uint2 Packed = f32tof16(value);
    return Packed.x | (Packed.y << 16);
}

float2 spvUnpackHalf2x16(uint value)
{
    return f16tof32(uint2(value & 0xffff, value >> 16));
}

uint spvBitfieldUExtract(uint Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint2 spvBitfieldUExtract(uint2 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint3 spvBitfieldUExtract(uint3 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

uint4 spvBitfieldUExtract(uint4 Base, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : ((1 << Count) - 1);
    return (Base >> Offset) & Mask;
}

int spvBitfieldSExtract(int Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int2 spvBitfieldSExtract(int2 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int2 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int3 spvBitfieldSExtract(int3 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int3 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int4 spvBitfieldSExtract(int4 Base, int Offset, int Count)
{
    int Mask = Count == 32 ? -1 : ((1 << Count) - 1);
    int4 Masked = (Base >> Offset) & Mask;
    int ExtendShift = (32 - Count) & 31;
    return (Masked << ExtendShift) >> ExtendShift;
}

int2 unswizzle8x8(uint index)
{
    uint y = spvBitfieldUExtract(index, 0, 1);
    uint x = spvBitfieldUExtract(index, 1, 2);
    y |= (spvBitfieldUExtract(index, 3, 2) << uint(1));
    x |= (spvBitfieldUExtract(index, 5, 1) << uint(2));
    return int2(int(x), int(y));
}

bool2 band(bool2 a, bool2 b)
{
    return bool2(a.x && b.x, a.y && b.y);
}

float2 generate_mirror_uv(inout int2 coord, bool even_x, bool even_y)
{
    bool2 param = bool2(even_x, even_y);
    bool2 param_1 = bool2(coord.x < int2(0, 0).x, coord.y < int2(0, 0).y);
    coord -= int2(band(param, param_1));
    coord += int2(1, 1);
    bool2 param_2 = bool2(!even_x, !even_y);
    bool2 param_3 = bool2(coord.x >= _150_resolution.x, coord.y >= _150_resolution.y);
    coord += int2(band(param_2, param_3));
    float2 uv = float2(coord) * _150_inv_resolution;
    return uv.yx;
}

void store_shared(uint y, uint x, float2 v)
{
    shared_block[y][x] = spvPackHalf2x16(v);
}

void write_shared_4x4(int2 coord, float4 texels0, float4 texels1, float4 texels2, float4 texels3)
{
    uint param = uint(coord.y + 0);
    uint param_1 = uint((2 * coord.x) + 0);
    float2 param_2 = float2(texels0.x, texels2.x);
    store_shared(param, param_1, param_2);
    uint param_3 = uint(coord.y + 0);
    uint param_4 = uint((2 * coord.x) + 1);
    float2 param_5 = float2(texels1.x, texels3.x);
    store_shared(param_3, param_4, param_5);
    uint param_6 = uint(coord.y + 0);
    uint param_7 = uint((2 * coord.x) + 2);
    float2 param_8 = float2(texels0.y, texels2.y);
    store_shared(param_6, param_7, param_8);
    uint param_9 = uint(coord.y + 0);
    uint param_10 = uint((2 * coord.x) + 3);
    float2 param_11 = float2(texels1.y, texels3.y);
    store_shared(param_9, param_10, param_11);
    uint param_12 = uint(coord.y + 1);
    uint param_13 = uint((2 * coord.x) + 0);
    float2 param_14 = float2(texels0.z, texels2.z);
    store_shared(param_12, param_13, param_14);
    uint param_15 = uint(coord.y + 1);
    uint param_16 = uint((2 * coord.x) + 1);
    float2 param_17 = float2(texels1.z, texels3.z);
    store_shared(param_15, param_16, param_17);
    uint param_18 = uint(coord.y + 1);
    uint param_19 = uint((2 * coord.x) + 2);
    float2 param_20 = float2(texels0.w, texels2.w);
    store_shared(param_18, param_19, param_20);
    uint param_21 = uint(coord.y + 1);
    uint param_22 = uint((2 * coord.x) + 3);
    float2 param_23 = float2(texels1.w, texels3.w);
    store_shared(param_21, param_22, param_23);
}

void load_image_with_apron()
{
    int2 base_coord = (int2(gl_WorkGroupID.xy) * int2(16, 16)) - int2(2, 2);
    uint param = local_index;
    int2 local_coord0 = int2(2, 2) * unswizzle8x8(param);
    int2 coord0 = base_coord + local_coord0;
    int2 param_1 = coord0;
    bool param_2 = true;
    bool param_3 = true;
    float2 _355 = generate_mirror_uv(param_1, param_2, param_3);
    float4 texels0 = uTexture.GatherRed(_uTexture_sampler, float3(_355, 0.0f)).wxzy;
    int2 param_4 = coord0;
    bool param_5 = false;
    bool param_6 = true;
    float2 _370 = generate_mirror_uv(param_4, param_5, param_6);
    float4 texels1 = uTexture.GatherRed(_uTexture_sampler, float3(_370, 2.0f)).wxzy;
    int2 param_7 = coord0;
    bool param_8 = true;
    bool param_9 = false;
    float2 _383 = generate_mirror_uv(param_7, param_8, param_9);
    float4 texels2 = uTexture.GatherRed(_uTexture_sampler, float3(_383, 1.0f)).wxzy;
    int2 param_10 = coord0;
    bool param_11 = false;
    bool param_12 = false;
    float2 _396 = generate_mirror_uv(param_10, param_11, param_12);
    float4 texels3 = uTexture.GatherRed(_uTexture_sampler, float3(_396, 3.0f)).wxzy;
    int2 param_13 = local_coord0;
    float4 param_14 = texels0;
    float4 param_15 = texels1;
    float4 param_16 = texels2;
    float4 param_17 = texels3;
    write_shared_4x4(param_13, param_14, param_15, param_16, param_17);
    int2 local_coord_horiz = int2(int(16u + (2u * (local_index % 2u))), int(2u * (local_index / 2u)));
    if (local_coord_horiz.y < 20)
    {
        int2 param_18 = base_coord + local_coord_horiz;
        bool param_19 = true;
        bool param_20 = true;
        float2 _439 = generate_mirror_uv(param_18, param_19, param_20);
        texels0 = uTexture.GatherRed(_uTexture_sampler, float3(_439, 0.0f)).wxzy;
        int2 param_21 = base_coord + local_coord_horiz;
        bool param_22 = false;
        bool param_23 = true;
        float2 _452 = generate_mirror_uv(param_21, param_22, param_23);
        texels1 = uTexture.GatherRed(_uTexture_sampler, float3(_452, 2.0f)).wxzy;
        int2 param_24 = base_coord + local_coord_horiz;
        bool param_25 = true;
        bool param_26 = false;
        float2 _465 = generate_mirror_uv(param_24, param_25, param_26);
        texels2 = uTexture.GatherRed(_uTexture_sampler, float3(_465, 1.0f)).wxzy;
        int2 param_27 = base_coord + local_coord_horiz;
        bool param_28 = false;
        bool param_29 = false;
        float2 _478 = generate_mirror_uv(param_27, param_28, param_29);
        texels3 = uTexture.GatherRed(_uTexture_sampler, float3(_478, 3.0f)).wxzy;
        int2 param_30 = local_coord_horiz;
        float4 param_31 = texels0;
        float4 param_32 = texels1;
        float4 param_33 = texels2;
        float4 param_34 = texels3;
        write_shared_4x4(param_30, param_31, param_32, param_33, param_34);
    }
    int2 local_coord_vert = local_coord_horiz.yx;
    if (local_coord_vert.x < 16)
    {
        int2 param_35 = base_coord + local_coord_vert;
        bool param_36 = true;
        bool param_37 = true;
        float2 _510 = generate_mirror_uv(param_35, param_36, param_37);
        texels0 = uTexture.GatherRed(_uTexture_sampler, float3(_510, 0.0f)).wxzy;
        int2 param_38 = base_coord + local_coord_vert;
        bool param_39 = false;
        bool param_40 = true;
        float2 _523 = generate_mirror_uv(param_38, param_39, param_40);
        texels1 = uTexture.GatherRed(_uTexture_sampler, float3(_523, 2.0f)).wxzy;
        int2 param_41 = base_coord + local_coord_vert;
        bool param_42 = true;
        bool param_43 = false;
        float2 _536 = generate_mirror_uv(param_41, param_42, param_43);
        texels2 = uTexture.GatherRed(_uTexture_sampler, float3(_536, 1.0f)).wxzy;
        int2 param_44 = base_coord + local_coord_vert;
        bool param_45 = false;
        bool param_46 = false;
        float2 _549 = generate_mirror_uv(param_44, param_45, param_46);
        texels3 = uTexture.GatherRed(_uTexture_sampler, float3(_549, 3.0f)).wxzy;
        int2 param_47 = local_coord_vert;
        float4 param_48 = texels0;
        float4 param_49 = texels1;
        float4 param_50 = texels2;
        float4 param_51 = texels3;
        write_shared_4x4(param_47, param_48, param_49, param_50, param_51);
    }
    GroupMemoryBarrierWithGroupSync();
}

float2 load_shared(uint y, uint x)
{
    return spvUnpackHalf2x16(shared_block[y][x]);
}

void inverse_transform8x2()
{
    int2 local_coord = int2(int(8u * (local_index % 4u)), int(local_index / 4u));
    float2 values[16];
    for (int i = 0; i < 16; i += 2)
    {
        uint param = uint(local_coord.y);
        uint param_1 = uint((local_coord.x + i) + 0);
        float2 v0 = load_shared(param, param_1);
        uint param_2 = uint(local_coord.y);
        uint param_3 = uint((local_coord.x + i) + 1);
        float2 v1 = load_shared(param_2, param_3);
        values[i + 0] = v0 * 1.23017406463623046875f;
        values[i + 1] = v1 * 0.812893092632293701171875f;
    }
    for (int i_1 = 2; i_1 < 15; i_1 += 2)
    {
        values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * 0.4435068666934967041015625f);
    }
    for (int i_2 = 3; i_2 < 14; i_2 += 2)
    {
        values[i_2] -= ((values[i_2 - 1] + values[i_2 + 1]) * 0.88291108608245849609375f);
    }
    for (int i_3 = 4; i_3 < 13; i_3 += 2)
    {
        values[i_3] -= ((values[i_3 - 1] + values[i_3 + 1]) * (-0.052980117499828338623046875f));
    }
    for (int i_4 = 5; i_4 < 12; i_4 += 2)
    {
        values[i_4] -= ((values[i_4 - 1] + values[i_4 + 1]) * (-1.58613431453704833984375f));
    }
    GroupMemoryBarrierWithGroupSync();
    for (int i_5 = 2; i_5 < 6; i_5++)
    {
        float2 a = values[(2 * i_5) + 0];
        float2 b = values[(2 * i_5) + 1];
        float2 t0 = float2(a.x, b.x);
        float2 t1 = float2(a.y, b.y);
        int y_coord = (local_coord.x >> 1) + (i_5 - 2);
        uint param_4 = uint(y_coord);
        uint param_5 = uint((2 * local_coord.y) + 0);
        float2 param_6 = t0;
        store_shared(param_4, param_5, param_6);
        uint param_7 = uint(y_coord);
        uint param_8 = uint((2 * local_coord.y) + 1);
        float2 param_9 = t1;
        store_shared(param_7, param_8, param_9);
    }
}

void inverse_transform4x2(bool active_lane, int y_offset)
{
    int2 local_coord = int2(int(4u * (local_index % 8u)), int((local_index / 8u) + uint(y_offset)));
    float2 values[12];
    if (active_lane)
    {
        for (int i = 0; i < 12; i += 2)
        {
            uint param = uint(local_coord.y);
            uint param_1 = uint((local_coord.x + i) + 0);
            float2 v0 = load_shared(param, param_1);
            uint param_2 = uint(local_coord.y);
            uint param_3 = uint((local_coord.x + i) + 1);
            float2 v1 = load_shared(param_2, param_3);
            values[i + 0] = v0 * 1.23017406463623046875f;
            values[i + 1] = v1 * 0.812893092632293701171875f;
        }
        for (int i_1 = 2; i_1 < 11; i_1 += 2)
        {
            values[i_1] -= ((values[i_1 - 1] + values[i_1 + 1]) * 0.4435068666934967041015625f);
        }
        for (int i_2 = 3; i_2 < 10; i_2 += 2)
        {
            values[i_2] -= ((values[i_2 - 1] + values[i_2 + 1]) * 0.88291108608245849609375f);
        }
        for (int i_3 = 4; i_3 < 9; i_3 += 2)
        {
            values[i_3] -= ((values[i_3 - 1] + values[i_3 + 1]) * (-0.052980117499828338623046875f));
        }
        for (int i_4 = 5; i_4 < 8; i_4 += 2)
        {
            values[i_4] -= ((values[i_4 - 1] + values[i_4 + 1]) * (-1.58613431453704833984375f));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (active_lane)
    {
        for (int i_5 = 2; i_5 < 4; i_5++)
        {
            float2 a = values[(2 * i_5) + 0];
            float2 b = values[(2 * i_5) + 1];
            float2 t0 = float2(a.x, b.x);
            float2 t1 = float2(a.y, b.y);
            int y_coord = (local_coord.x >> 1) + (i_5 - 2);
            uint param_4 = uint(y_coord);
            uint param_5 = uint((2 * local_coord.y) + 0);
            float2 param_6 = t0;
            store_shared(param_4, param_5, param_6);
            uint param_7 = uint(y_coord);
            uint param_8 = uint((2 * local_coord.y) + 1);
            float2 param_9 = t1;
            store_shared(param_7, param_8, param_9);
        }
    }
}

void comp_main()
{
    local_index = gl_LocalInvocationIndex;
    load_image_with_apron();
    inverse_transform8x2();
    bool param = local_index < 32u;
    int param_1 = 16;
    inverse_transform4x2(param, param_1);
    GroupMemoryBarrierWithGroupSync();
    inverse_transform8x2();
    GroupMemoryBarrierWithGroupSync();
    uint param_2 = local_index;
    int2 local_coord = unswizzle8x8(param_2);
    for (int y = local_coord.y; y < 16; y += 8)
    {
        for (int x = local_coord.x; x < 32; x += 8)
        {
            uint param_3 = uint(y);
            uint param_4 = uint(x);
            float2 v = load_shared(param_3, param_4);
            if (DCShift)
            {
                v += 0.5f.xx;
            }
            uOutput[int3(int2((2 * y) + 0, x) + (int2(32, 32) * int2(gl_WorkGroupID.yx)), 0)] = v.xxxx;
            uOutput[int3(int2((2 * y) + 1, x) + (int2(32, 32) * int2(gl_WorkGroupID.yx)), 0)] = v.yyyy;
        }
    }
}

[numthreads(64, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
