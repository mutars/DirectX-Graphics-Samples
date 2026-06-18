// DlssMotionVectorsCS.hlsl
// Converts MiniEngine's packed R32_UINT velocity buffer into the unjittered RG16_FLOAT
// pixel-space motion vectors that DLSS expects. DLSS receives the jitter separately via
// InJitterOffset, so MVs must NOT include the jitter component.
//
// g_VelocityBuffer: R32_UINT, packed 3D velocity (current->previous pixel space).
// g_DLSSMotionBuffer: RW RG16_FLOAT, unjittered 2D pixel-space MV output.

#include "PixelPacking_Velocity.hlsli"

cbuffer DlssMVCB : register(b1)
{
    float JitterDeltaX;
    float JitterDeltaY;
    float _pad0;
    float _pad1;
}

Texture2D<uint>    g_VelocityBuffer   : register(t0);
RWTexture2D<float2> g_DLSSMotionBuffer : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    uint2 coord = DTid.xy;
    uint width, height;
    g_VelocityBuffer.GetDimensions(width, height);
    if (coord.x >= width || coord.y >= height)
        return;

    uint packed = g_VelocityBuffer[coord];
    float3 vel  = UnpackVelocity(packed);

    // vel.xy is current->previous in pixel space with jitter baked in.
    // Subtract the jitter delta so the MV is unjittered for DLSS.
    float2 mv = vel.xy - float2(JitterDeltaX, JitterDeltaY);

    g_DLSSMotionBuffer[coord] = mv;
}
