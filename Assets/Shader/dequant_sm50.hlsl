static const uint3 gl_WorkGroupSize = uint3(128u, 1u, 1u);

static const int _176[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

ByteAddressBuffer payload_offsets : register(t1);
cbuffer Registers
{
    int2 registers_resolution : packoffset(c0);
    int registers_output_layer : packoffset(c0.z);
    int registers_block_offset_32x32 : packoffset(c0.w);
    int registers_block_stride_32x32 : packoffset(c1);
};

Buffer<uint4> PayloadU8 : register(t4);
Buffer<uint4> PayloadU16 : register(t3);
Buffer<uint4> PayloadU32 : register(t2);
RWTexture2DArray<float4> uDequantImg : register(u0);

static uint3 gl_WorkGroupID;
static uint gl_LocalInvocationIndex;
struct SPIRV_Cross_Input
{
    uint3 gl_WorkGroupID : SV_GroupID;
    uint gl_LocalInvocationIndex : SV_GroupIndex;
};

groupshared uint shared_scan_scratch[128];
groupshared uint shared_sign_offset;
groupshared uint shared_plane_byte_offsets[16];

uint spvBitfieldInsert(uint Base, uint Insert, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : (((1u << Count) - 1) << (Offset & 31));
    return (Base & ~Mask) | ((Insert << Offset) & Mask);
}

uint2 spvBitfieldInsert(uint2 Base, uint2 Insert, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : (((1u << Count) - 1) << (Offset & 31));
    return (Base & ~Mask) | ((Insert << Offset) & Mask);
}

uint3 spvBitfieldInsert(uint3 Base, uint3 Insert, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : (((1u << Count) - 1) << (Offset & 31));
    return (Base & ~Mask) | ((Insert << Offset) & Mask);
}

uint4 spvBitfieldInsert(uint4 Base, uint4 Insert, uint Offset, uint Count)
{
    uint Mask = Count == 32 ? 0xffffffff : (((1u << Count) - 1) << (Offset & 31));
    return (Base & ~Mask) | ((Insert << Offset) & Mask);
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

uint read_payload_u16(int coord)
{
    uint _104 = PayloadU16.Load(coord).x;
    return _104;
}

uint read_payload_u8(int coord)
{
    uint _94 = PayloadU8.Load(coord).x;
    return _94;
}

uint workgroup_inclusive_add(inout uint v, uint local_index)
{
    shared_scan_scratch[local_index] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint i = 1u; i < 128u; i *= 2u)
    {
        uint up = 0u;
        if (local_index >= i)
        {
            up = shared_scan_scratch[local_index - i];
        }
        GroupMemoryBarrierWithGroupSync();
        if (local_index >= i)
        {
            v += up;
            shared_scan_scratch[local_index] = v;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    return v;
}

float2x4 decode_payload(uint code_word, uint q_bits, uint offset, uint block_index)
{
    bool empty_block = code_word == 0u;
    if (empty_block)
    {
        return float2x4(0.0f.xxxx, 0.0f.xxxx);
    }
    int bit_offset = 2 * int(block_index);
    uint lsbs = code_word & 21845u;
    uint msbs = code_word & 43690u;
    uint msbs_shift = msbs >> uint(1);
    msbs |= msbs_shift;
    uint byte_offset = (uint(int(countbits(spvBitfieldUExtract(lsbs, 0, bit_offset))) + int(countbits(spvBitfieldUExtract(msbs, 0, bit_offset)))) + (q_bits * block_index)) + offset;
    int param = int(byte_offset);
    uint _payload = read_payload_u8(param);
    uint local_control_word = spvBitfieldUExtract(code_word, bit_offset, 2);
    int decoded_abs[8] = _176;
    int plane_iterations = int(q_bits + local_control_word);
    int _184 = plane_iterations - 1;
    for (int q = _184; q >= 0; q--)
    {
        for (int b = 0; b < 8; b++)
        {
            int decoded = int(spvBitfieldUExtract(_payload, b, 1));
            decoded_abs[b] = int(spvBitfieldInsert(decoded_abs[b], decoded, q, 1));
        }
        byte_offset++;
        int param_1 = int(byte_offset);
        _payload = read_payload_u8(param_1);
    }
    float2x4 m;
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 2; j++)
        {
            float v = float(decoded_abs[(i * 2) + j]);
            if (v != 0.0f)
            {
                v += 0.5f;
            }
            m[j][i] = v;
        }
    }
    return m;
}

float decode_quant(uint quant_code)
{
    int e = 4 - int(quant_code >> uint(3));
    int m = int(quant_code) & 7;
    float inv_quant = 1.1920928955078125e-07f * float((8 + m) * (1 << (20 + e)));
    return inv_quant;
}

float decode_quant_scale(uint code)
{
    return (float(code) / 8.0f) + 0.25f;
}

uint read_payload_u32(int coord)
{
    return PayloadU32.Load(coord).x;
}

void comp_main()
{
    uint local_index = gl_LocalInvocationIndex;
    int block_index_32x32 = int((uint(registers_block_offset_32x32) + (gl_WorkGroupID.y * uint(registers_block_stride_32x32))) + gl_WorkGroupID.x);
    uint block_local_index = spvBitfieldUExtract(local_index, 0, 3);
    uint block_x = spvBitfieldUExtract(local_index, 3, 2);
    uint block_y = spvBitfieldUExtract(local_index, 5, 2);
    uint linear_block = (block_y * 4u) + block_x;
    uint param = block_local_index << uint(3);
    int2 local_coord = unswizzle8x8(param);
    int2 coord = int2(gl_WorkGroupID.xy) * int2(32, 32);
    coord += (int2(8, 8) * int2(int(block_x), int(block_y)));
    coord += local_coord;
    uint offset_u32 = payload_offsets.Load(block_index_32x32 * 4 + 0);
    if (offset_u32 == 4294967295u)
    {
        for (int j = 0; j < 2; j++)
        {
            for (int i = 0; i < 4; i++)
            {
                uDequantImg[int3(coord + int2(i, j), registers_output_layer)] = 0.0f.xxxx;
            }
        }
        return;
    }
    int param_1 = 2 * int(offset_u32);
    uint ballot = read_payload_u16(param_1);
    int param_2 = (4 * int(offset_u32)) + 4;
    uint q_code = read_payload_u8(param_2);
    uint plane_byte_cost = 0u;
    if (local_index < 16u)
    {
        uint control_word = 0u;
        uint q_bits = 0u;
        if (spvBitfieldUExtract(ballot, int(local_index), 1) != 0u)
        {
            uint local_code_offset = uint(int(countbits(spvBitfieldUExtract(ballot, 0, int(local_index)))));
            int param_3 = int(((offset_u32 * 2u) + 4u) + local_code_offset);
            control_word = read_payload_u16(param_3);
            int param_4 = int((((offset_u32 * 4u) + 8u) + uint(int(countbits(ballot)) * 2)) + local_code_offset);
            q_bits = read_payload_u8(param_4) & 15u;
        }
        uint lsbs = control_word & 21845u;
        uint msbs = control_word & 43690u;
        uint msbs_shift = msbs >> uint(1);
        msbs |= msbs_shift;
        plane_byte_cost = uint(int(countbits(lsbs)) + int(countbits(msbs))) + (q_bits * 8u);
    }
    uint param_5 = plane_byte_cost;
    uint param_6 = local_index;
    uint _541 = workgroup_inclusive_add(param_5, param_6);
    uint plane_byte_scan = _541;
    if (local_index < 16u)
    {
        uint byte_scan = (((offset_u32 * 4u) + 8u) + uint(3 * int(countbits(ballot)))) + plane_byte_scan;
        if (local_index == 15u)
        {
            shared_sign_offset = 8u * byte_scan;
        }
        shared_plane_byte_offsets[local_index] = byte_scan - plane_byte_cost;
    }
    GroupMemoryBarrierWithGroupSync();
    float2x4 v;
    int significant_count;
    if (spvBitfieldUExtract(ballot, int(linear_block), 1) != 0u)
    {
        uint local_code_offset_1 = uint(int(countbits(spvBitfieldUExtract(ballot, 0, int(linear_block)))));
        int param_7 = int(((offset_u32 * 2u) + 4u) + local_code_offset_1);
        uint control_word_1 = read_payload_u16(param_7);
        int param_8 = int((((offset_u32 * 4u) + 8u) + uint(int(countbits(ballot)) * 2)) + local_code_offset_1);
        uint control_word2 = read_payload_u8(param_8);
        uint param_9 = control_word_1;
        uint param_10 = control_word2 & 15u;
        uint param_11 = shared_plane_byte_offsets[linear_block];
        uint param_12 = block_local_index;
        v = decode_payload(param_9, param_10, param_11, param_12);
        significant_count = 0;
        for (int j_1 = 0; j_1 < 2; j_1++)
        {
            for (int i_1 = 0; i_1 < 4; i_1++)
            {
                significant_count += int(v[j_1][i_1] != 0.0f);
            }
        }
        uint param_13 = q_code;
        float q = decode_quant(param_13);
        uint param_14 = spvBitfieldUExtract(control_word2, 4, 4);
        float inv_scale = q * decode_quant_scale(param_14);
        v = v * inv_scale;
    }
    else
    {
        v = float2x4(0.0f.xxxx, 0.0f.xxxx);
        significant_count = 0;
    }
    uint param_15 = uint(significant_count);
    uint param_16 = local_index;
    uint _672 = workgroup_inclusive_add(param_15, param_16);
    uint significant_scan_wg = _672;
    uint sign_offset = (shared_sign_offset + significant_scan_wg) - uint(significant_count);
    int param_17 = int((sign_offset / 32u) + 0u);
    uint sign_word = read_payload_u32(param_17);
    int param_18 = int((sign_offset / 32u) + 1u);
    uint sign_word_upper = read_payload_u32(param_18);
    uint masked_sign_offset = sign_offset & 31u;
    if (masked_sign_offset != 0u)
    {
        sign_word = sign_word >> masked_sign_offset;
        sign_word |= (sign_word_upper << (32u - masked_sign_offset));
    }
    int sign_counter = 0;
    for (int i_2 = 0; i_2 < 4; i_2++)
    {
        for (int j_2 = 0; j_2 < 2; j_2++)
        {
            if (v[j_2][i_2] != 0.0f)
            {
                v[j_2][i_2] *= (1.0f - (2.0f * float(spvBitfieldUExtract(sign_word, sign_counter, 1))));
                sign_counter++;
            }
        }
    }
    for (int j_3 = 0; j_3 < 2; j_3++)
    {
        for (int i_3 = 0; i_3 < 4; i_3++)
        {
            uDequantImg[int3(coord + int2(i_3, j_3), registers_output_layer)] = v[j_3][i_3].xxxx;
        }
    }
}

[numthreads(128, 1, 1)]
void main(SPIRV_Cross_Input stage_input)
{
    gl_WorkGroupID = stage_input.gl_WorkGroupID;
    gl_LocalInvocationIndex = stage_input.gl_LocalInvocationIndex;
    comp_main();
}
