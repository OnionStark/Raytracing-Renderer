/**********************************************************************************************************************
# Copyright (c) 2018, NVIDIA CORPORATION. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
# following conditions are met:
#  * Redistributions of code must retain the copyright notice, this list of conditions and the following disclaimer.
#  * Neither the name of NVIDIA CORPORATION nor the names of its contributors may be used to endorse or promote products
#    derived from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT
# SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
# OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
# ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************************************************************/

// Some shared Falcor stuff for talking between CPU and GPU code
#include "HostDeviceSharedMacros.h"

// Include and import common Falcor utilities and data structures
__import Raytracing;                   // Shared ray tracing specific functions & data
__import ShaderCommon;                 // Shared shading data structures
__import Shading;                      // Shading functions, etc       

// Payload for our primary rays.  This shader doesn't actually use the data, but it is currently
//    required to use a user-defined payload while tracing a ray.  So define a simple one.
struct SimpleRayPayload
{
	float Occlusion;
};


shared RWByteAddressBuffer ExposeVertex;
shared RWByteAddressBuffer dPosition;
shared RWByteAddressBuffer dNormal;
shared Texture2D<float4> pNoise;
shared SamplerState pSampler;

// How do we generate the rays that we trace?
[shader("raygeneration")]
void GBufferRayGen()
{
	float3 rayOrigin =  asfloat(dPosition.Load3((DispatchRaysIndex().x*3)*4)); 
	//float3 rayOrigin =  float3(0,0,0);               
	float3 rayDir = float3(0.0f,1.0f,0.0f);  

	// Initialize a ray structure for our ray tracer
	RayDesc ray;
	ray.Origin    = rayOrigin;      // Start our ray at the world-space camera position
	ray.Direction = normalize(rayDir); // Our ray direction; normalizing this is often wise
	ray.TMin      = 0.001f;              // Start at 0.0; for camera, no danger of self-intersection
	ray.TMax      = 1e+38f;            // Maximum distance to look for a ray hit

	// Initialize our ray payload (a per-ray, user-definable structure).  
	SimpleRayPayload rayData = { 0.0f };
	// Trace our ray
	TraceRay(gRtScene,                        // A Falcor built-in containing the raytracing acceleration structure
		RAY_FLAG_NONE,  // Ray flags.  (Here, we will skip hits with back-facing triangles)
		0xFF,                                 // Instance inclusion mask.  0xFF => no instances discarded from this mask
		0,                                    // Hit group to index (i.e., when intersecting, call hit shader #0)
		hitProgramCount,                      // Number of hit groups ('hitProgramCount' is built-in from Falcor with the right number)
		0,                                    // Miss program index (i.e., when missing, call miss shader #0)
		ray,                                  // Data structure describing the ray to trace
		rayData);                             // Our user-defined ray payload structure to store intermediate results
	ExposeVertex.Store(DispatchRaysIndex().x*4,asint(rayData.Occlusion));
}




// What code is executed when our ray misses all geometry?
[shader("miss")]
void PrimaryMiss(inout SimpleRayPayload rayData)
{
	// Store the background color into our diffuse material buffer
	rayData.Occlusion = 1.0f;
	float inc = dot(normalize(asfloat(dNormal.Load3(DispatchRaysIndex().x*3*4))), float3(0,1,0));
	//inc = inc + pNoise.Sample(pSampler, float2(WorldRayOrigin().x,WorldRayOrigin().z)).z * 0.4f;
	ExplicitLodTextureSampler lodSampler = { 0 };
	inc = inc + sampleTexture(pNoise, pSampler, float2(WorldRayOrigin().x,WorldRayOrigin().z), float4(1,1,1,1), 2, lodSampler).z * 0.4f;
	rayData.Occlusion = rayData.Occlusion * inc;
	dPosition.Store(DispatchRaysIndex().x*4*3+4, asint(WorldRayOrigin().y+rayData.Occlusion*0.2f));
}



// What code is executed when our ray hits a potentially transparent surface?
[shader("anyhit")]
void PrimaryAnyHit(inout SimpleRayPayload rayData, BuiltInTriangleIntersectionAttributes attribs)
{
	rayData.Occlusion = 0.0f;
    
}

// What code is executed when we have a new closest hitpoint?
[shader("closesthit")]
void PrimaryClosestHit(inout SimpleRayPayload rayData, BuiltInTriangleIntersectionAttributes attribs)
{
	// Which pixel spawned our ray?
	rayData.Occlusion = 0.0f;
	//PrimitiveDirtyBuffer.Store(PrimitiveIndex()*4, asint(1));
}
