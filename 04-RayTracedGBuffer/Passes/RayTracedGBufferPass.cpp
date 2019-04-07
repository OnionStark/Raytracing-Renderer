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

#include "RayTracedGBufferPass.h"

// Some global vars, used to simplify changing shader location & entry points
namespace {
	// Where is our shader located?
	const char* kFileRayTrace = "Tutorial04\\rayTracedGBuffer.rt.hlsl";

	// What function names are used for the shader entry points for various shaders?
	const char* kEntryPointRayGen       = "GBufferRayGen";
	const char* kEntryPointMiss0        = "PrimaryMiss";
	const char* kEntryPrimaryAnyHit     = "PrimaryAnyHit";
	const char* kEntryPrimaryClosestHit = "PrimaryClosestHit";
};

bool RayTracedGBufferPass::initialize(RenderContext* pRenderContext, ResourceManager::SharedPtr pResManager)
{
	// Stash a copy of our resource manager so we can get rendering resources
	mpResManager = pResManager;

	// We need a bunch of textures to store our G-buffer.  Ask for a list of them.  They all get the same 
	//     format (in this case, the default, RGBA32F) and size (in this case, the default, screen sized)
	mpResManager->requestTextureResources({ "WorldPosition", "WorldNormal", "MaterialDiffuse", 
		                                    "MaterialSpecRough", "MaterialExtraParams", "Emissive" });

	// Set the default scene to load
	//mpResManager->setDefaultSceneName("Data/pink_room/pink_room.fscene");
	mpResManager->setDefaultSceneName("Data/cube/cube.fscene");

	// Create our wrapper around a ray tracing pass.  Specify our ray generation shader and ray-specific shaders
	mpRays = RayLaunch::create(kFileRayTrace, kEntryPointRayGen);
	mpRays->addMissShader(kFileRayTrace, kEntryPointMiss0);                             // Add miss shader #0 
	mpRays->addHitShader(kFileRayTrace, kEntryPrimaryClosestHit, kEntryPrimaryAnyHit);  // Add hit group #0

	// Now that we've passed all our shaders in, compile.  If we already have our scene, let it know what scene to use.
	mpRays->compileRayProgram();
	if (mpScene) mpRays->setScene(mpScene);

    return true;
}

void RayTracedGBufferPass::initScene(RenderContext* pRenderContext, Scene::SharedPtr pScene)
{
	// Stash a copy of the scene.  To use our DXR wrappers, we currently require a Falcor::RtScene 
	//    (as Falcor::Scene currently works only for rasterization).  This is a distinction that will 
	//    go away as DirectX Raytracing becomes available widely.  RtScene is derived from Scene, and 
	//    all scenes loaded in this set of tutorial apps are RtScenes, so we just do a cast here.
	mpScene = std::dynamic_pointer_cast<RtScene>(pScene);

	// Pass our scene to our ray tracer (if initialized)
	if (mpRays) mpRays->setScene(mpScene);
	
	//创建primitive的状态位，0为未变动，1为待变动，2为已变动
	std::vector<uint32_t> temp;
	temp.resize(mpScene->getModel(0)->getPrimitiveCount());
	std::generate(temp.begin(), temp.end(), [] {return 0; });
	PrimitiveDirtyBuffer = Buffer::create(mpScene->getModel(0)->getPrimitiveCount() * sizeof(uint32_t), Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None, temp.data());
	auto HitVar = mpRays->getGlobalVars();
	bool vertify = HitVar->setRawBuffer("PrimitiveDirtyBuffer", PrimitiveDirtyBuffer);
	

}

void RayTracedGBufferPass::execute(RenderContext* pRenderContext)
{
	// Check that we're ready to render
	if (!mpRays || !mpRays->readyToRender()) return;

	// Load our textures, but ask the resource manager to clear them to black before returning them
	Texture::SharedPtr wsPos    = mpResManager->getClearedTexture("WorldPosition",       vec4(0, 0, 0, 0));
	Texture::SharedPtr wsNorm   = mpResManager->getClearedTexture("WorldNormal",         vec4(0, 0, 0, 0));
	Texture::SharedPtr matDif   = mpResManager->getClearedTexture("MaterialDiffuse",     vec4(0, 0, 0, 0));
	Texture::SharedPtr matSpec  = mpResManager->getClearedTexture("MaterialSpecRough",   vec4(0, 0, 0, 0));
	Texture::SharedPtr matExtra = mpResManager->getClearedTexture("MaterialExtraParams", vec4(0, 0, 0, 0));
	Texture::SharedPtr matEmit  = mpResManager->getClearedTexture("Emissive",            vec4(0, 0, 0, 0));

	//auto Var = mpRays->getHitVars(0)[0];

	TestList = StructuredBuffer::create("testList",mpRays->getHitVars(0)[0]->getVars()->getDefaultBlock()->getReflection()->getResource("testList").get()->getType()->unwrapArray()->asResourceType()->inherit_shared_from_this::shared_from_this(), (int)mpScene->getModel(0)->getPrimitiveCount());
	std::vector<int> temp;
	temp.resize(mpScene->getModel(0)->getPrimitiveCount());
	uint32_t counter = 0;
	TestList->getUAVCounter()->updateData(&counter, 0, sizeof(uint32_t));
	std::generate(temp.begin(), temp.end(), [] {return -1; });
	TestList->setBlob(temp.data(), 0, temp.size()*sizeof(int));

	// Now we'll send our parameters down to our ray tracing shaders

	// Pass our background color down to miss shader #0
	auto missVars = mpRays->getMissVars(0);
	missVars["MissShaderCB"]["gBgColor"] = mBgColor;  // What color to use as a background?
	missVars["gMatDif"] = matDif;                     // Where do we store the bg color? (in the diffuse texture)

	// Cycle through all geometry instances, bind our g-buffer textures to the hit shaders for each instance.
	// Note:  There is a different binding point for each pair {instance, hit group}, so variables used inside
	//        the hit group need to be bound per-instance.  If these variables do not change, as in this case,
	//        they could also be bound as a global variable (visibile in *all* shaders) for better performance
	for (auto pVars : mpRays->getHitVars(0))   // Cycle through geom instances for hit group #0
	{
		pVars["gWsPos"] = wsPos;
		pVars["gWsNorm"] = wsNorm;
		pVars["gMatDif"] = matDif;
		pVars["gMatSpec"] = matSpec;
		pVars["gMatExtra"] = matExtra;
		pVars["gMatEmissive"] = matEmit;

		pVars->setStructuredBuffer("testList", TestList);

	}
	
	// Launch our ray tracing
	mpRays->execute( pRenderContext, mpResManager->getScreenSize() );
	if (mpScene) {
		ReadBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0));
		OutputDebugStringA("\n");
		ReadBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(1));
		OutputDebugStringA("\n");
		ReadBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(2));
		OutputDebugStringA("\n");
		ReadBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(3));
		OutputDebugStringA("\n");
		ReadBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getIndexBuffer(),true);
		/*int32_t *list= new int32_t[mpScene->getModel(0)->getPrimitiveCount()];
		TestList->readBlob(list, 0, mpScene->getModel(0)->getPrimitiveCount()*4);
		for (uint i = 0; i < mpScene->getModel(0)->getPrimitiveCount(); i++) {
			char chInput[512];
				sprintf(chInput, "%d ", list[i]);
				OutputDebugStringA(chInput);
			OutputDebugStringA("\n");
		}*/
		OutputDebugStringA("\n");
	}
	if (!hasDone) {
		TestWriteBuffer(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0));
		hasDone = true;

	}

}

void RayTracedGBufferPass::ReadBuffer(const Buffer::SharedPtr& buffer,bool isIndecies)
{
	float* vPosition;
	int* vIndecies;
	if (!isIndecies) {
		vPosition = new float[buffer->getSize() / 4];
		vPosition = (float*)buffer->map(Buffer::MapType::Read);
	}
	else {
		vIndecies = new int[buffer->getSize() / 4];
		vIndecies = (int*)buffer->map(Buffer::MapType::Read);
	}
	for (int j = 0; j < buffer->getSize()/4/3; j++) 
	{
		for (int i = 0; i < 3&& (j * 3 + i)< buffer->getSize() / 4; i++)
		{
			//std::cout << vPosition[j * 3 + i] << " ";
			char chInput[512];
			if(!isIndecies)
			sprintf(chInput, "%f ", vPosition[j * 3 + i]);
			else
			sprintf(chInput, "%d ", vIndecies[j * 3 + i]);

			OutputDebugStringA(chInput);
		}
		OutputDebugStringA("\n");
	}
	buffer->unmap();

}

void RayTracedGBufferPass::TestWriteBuffer(const Buffer::SharedPtr& buffer) {
	//更新顶点数组
	float* data = new float[4];
	data[3] = 1.5f;
	float i = 3.f;
	size_t off = sizeof(float) * 4;
	buffer->updateData(&i, 16, sizeof(float));
	dynamic_cast<RtModel*>(mpScene->getModel(0).get())->updateBottomLevelData();
	mpScene->getInstanceCount(1);
	char chInput[512];
	//sprintf(chInput, "%f ", data[0]);
	sprintf(chInput, "%d ", (int)sizeof(float));
	OutputDebugStringA(chInput);
	OutputDebugStringA("\n");
}
