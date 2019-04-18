#include "PreProcess.h"
#include <glm/gtc/noise.hpp>
namespace {
	// Raytracing shader
	const char* kFileRayTrace = "CommonPasses\\preProcessRaytracing.rt.hlsl";
	// What function names are used for the shader entry points for various shaders?
	const char* kEntryPointRayGen = "GBufferRayGen";
	const char* kEntryPointMiss0 = "PrimaryMiss";
	const char* kEntryPrimaryAnyHit = "PrimaryAnyHit";
	const char* kEntryPrimaryClosestHit = "PrimaryClosestHit";

	//Raster Shader
	const char *kGbufVertShader = "CommonPasses\\PreProcessingGBuffer.vs.hlsl";
	const char *kGbufFragShader = "CommonPasses\\PreProcessingGBuffer.ps.hlsl";


}


bool PreProcess::initialize(RenderContext * pRenderContext, ResourceManager::SharedPtr pResManager)
{
	mpResManager = pResManager;

	//设置默认场景
	mpResManager->setDefaultSceneName("Data/cube/cube.fscene");

	// 设置光线追踪shader
	mpRays = RayLaunch::create(kFileRayTrace, kEntryPointRayGen);
	mpRays->addMissShader(kFileRayTrace, kEntryPointMiss0);                             // Add miss shader #0 
	mpRays->addHitShader(kFileRayTrace, kEntryPrimaryClosestHit, kEntryPrimaryAnyHit);  // Add hit group #0
	// Now that we've passed all our shaders in, compile.  If we already have our scene, let it know what scene to use.
	mpRays->compileRayProgram();
	if (mpRtScene) mpRays->setScene(mpRtScene);

	//设置光栅化shader
	mpResManager->requestTextureResources({ "WorldPosition", "WorldNormal", "MaterialDiffuse",
											"MaterialSpecRough", "MaterialExtraParams","Prediction" });

	// We also need a depth buffer to use when rendering our g-buffer.  Ask for one, with appropriate format and binding flags.
	mpResManager->requestTextureResource("Z-Buffer", ResourceFormat::D24UnormS8, ResourceManager::kDepthBufferFlags);

	

	// Since we're rasterizing, we need to define our raster pipeline state (though we use the defaults)
	mpGfxState = GraphicsState::create();

	// Create our wrapper for a scene-rasterization pass.
	mpRaster = RasterLaunch::createFromFiles(kGbufVertShader, kGbufFragShader);
	mpRaster->setScene(mpScene);

	//创建噪声图和噪声采样器
	noiseTex = createPerlinNoise(200, 200, 20.0f, 2.0f);
	Sampler::Desc samplerDesc;
	samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
	mpNoiseSampler = Sampler::create(samplerDesc);

	//共享噪声图
	mpResManager->manageTextureResource("Noise", noiseTex);

	return true;
}

void PreProcess::execute(RenderContext * pRenderContext)
{
	// Check that we're ready to render
	if (!mpRays || !mpRays->readyToRender()) return;

	//执行积雪预处理
	if (ProcessState == 1) {
		auto Vars = mpRays->getGlobalVars();
		Vars["pNoise"] = noiseTex;
		Vars->setSampler("pSampler", mpNoiseSampler);
		
		auto missVars = mpRays->getMissVars(0);
		missVars["PreProcessCB"]["coordScale"] = CoordScale;
		missVars["PreProcessCB"]["coordBias"] = Bias;
		missVars["PreProcessCB"]["yScale"] = yScale;
		RenderContext* pContext = gpDevice->getRenderContext().get();
		//pContext->copyResource(mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get(), OriginVertexBuffer.get());
		mpRays->execute(pRenderContext, uvec2(mpScene->getModel(0)->getVertexCount(),1));

		//执行顶点位移操作
		//这一步把位移后的顶点位置复制到原来的顶点数组内，但是RtScene的加速结构或许需要重新构建？

		pContext->copyResource(mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get(), ChangeVertexBuffer.get());
		pContext->flush(true);

		//更新加速结构
		dynamic_cast<RtModel*>(mpRtScene->getModel(0).get())->updateBottomLevelData();
		ProcessState++;
		//ReadBuffer(ExposeVertex, false);
		//ReadBuffer(ChangeVertexBuffer, false);
	}

	//debug生成的噪声图
	// Get a pointer to a Falcor texture resource for our output 
	/*Texture::SharedPtr outTex = mpResManager->getTexture(ResourceManager::kOutputChannel);
	if (!outTex) return;
	Texture::SharedPtr inTex = createPerlinNoise(200, 200, 15.0f, 2.0f);
	pRenderContext->blit(inTex->getSRV(), outTex->getRTV(),uvec4(-1),uvec4(800,400,1000,600));*/

	//生成GBuffer
	// Create a framebuffer for rendering.  (Creating once per frame is for simplicity, not performance).
	outputFbo = mpResManager->createManagedFbo(
		{ "WorldPosition", "WorldNormal", "MaterialDiffuse", "MaterialSpecRough", "MaterialExtraParams", "Prediction" }, // Names of color buffers
		"Z-Buffer");                                                                                      // Name of depth buffer
	if (!outputFbo) return;
	// Clear g-buffer.  Clear colors to black, depth to 1, stencil to 0, but then clear diffuse texture to our bg color
	pRenderContext->clearFbo(outputFbo.get(), vec4(0, 0, 0, 0), 1.0f, 0);
	pRenderContext->clearUAV(outputFbo->getColorTexture(2)->getUAV().get(), vec4(mBgColor, 1.0f));

	//设置rasterization管线变量
	auto mVars = mpRaster->getVars();
	mVars->setRawBuffer("PredictionList", ExposeVertex);
	mVars["PerFrameCB"]["ProcessState"] = ProcessState;

	mpRaster->execute(pRenderContext, mpGfxState, outputFbo);
}

void PreProcess::initScene(RenderContext * pRenderContext, Scene::SharedPtr pScene)
{
	if (pScene) {
		mpScene = pScene;
		mpRtScene = std::dynamic_pointer_cast<RtScene>(pScene);
	}
	if (mpRaster)
		mpRaster->setScene(mpScene);
	if (mpRays) mpRays->setScene(mpRtScene);

	//创建Expose缓存，默认为0
	std::vector<float> temp;
	temp.resize(mpRtScene->getModel(0)->getVertexCount());
	std::generate(temp.begin(), temp.end(), [] {return 0.0f; });
	ExposeVertex = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float), Resource::BindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None, temp.data());
	auto HitVar = mpRays->getGlobalVars();
	bool vertifyExpose = HitVar->setRawBuffer("ExposeVertex", ExposeVertex);

	//创建偏移后的顶点buffer
	ChangeVertexBuffer = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	RenderContext* pContext = gpDevice->getRenderContext().get();
	pContext->copyResource(ChangeVertexBuffer.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get());
	pContext->flush(true);

	//创建顶点的法向量buffer
	ChangeVertexNormal = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	pContext->copyResource(ChangeVertexNormal.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(1).get());
	pContext->flush(true);

	//保存原始的顶点buffer
	OriginVertexBuffer = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	pContext->copyResource(OriginVertexBuffer.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get());
	pContext->flush(true);

	//调试用，验证是否已经设置成功
	bool vertifyPosition = HitVar->setRawBuffer("dPosition", ChangeVertexBuffer);
	bool vertifyNormal = HitVar->setRawBuffer("dNormal", ChangeVertexNormal);
	//ReadBuffer(ChangeVertexBuffer, false);
}

void PreProcess::renderGui(Gui * pGui)
{
	//pGui->setCurrentWindowSize(220, 250);
	int Noisedirty = 0;
	pGui->addText("Set noise texture parameters:");
	Noisedirty |= (int)pGui->addFloatVar("Frquency", frequency, 1e-4f, 1e38f, frequency * 0.01f);
	Noisedirty |= (int)pGui->addFloatVar("Amplitude", amplitude, 1e-4f, 1e38f, amplitude *0.01f);

	int PreprocessDirty = 0;
	pGui->addText("Set preprocess parameters:");
	PreprocessDirty |= (int)pGui->addFloatVar("Coordinate Scale", CoordScale, 1e-4f, 1e38f, CoordScale * 0.01f);
	PreprocessDirty |= (int)pGui->addFloatVar("Coordinate Bias", Bias);
	PreprocessDirty |= (int)pGui->addFloatVar("y-axis Scale", yScale);
	PreprocessDirty |= (int)pGui->addFloatVar("Blender Threshold", threshold, 0.0f, 1.0f, 0.001f);

	if (Noisedirty || PreprocessDirty) {
		if (Noisedirty) {
			noiseTex = createPerlinNoise(200, 200, frequency, amplitude);
			mpResManager->manageTextureResource("Noise", noiseTex);
		}
		setRefreshFlag();
	}

	
	if (pGui->addButton("Do Snow Modeling", false)) {
		RenderContext* pContext = gpDevice->getRenderContext().get();
		pContext->copyResource(mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get(), OriginVertexBuffer.get());
		//pContext->copyResource(mpScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get(), OriginVertexBuffer.get());
		pContext->copyResource(ChangeVertexBuffer.get(), OriginVertexBuffer.get());
		pContext->flush(true);

		//更新加速结构
		dynamic_cast<RtModel*>(mpRtScene->getModel(0).get())->updateBottomLevelData();
		ProcessState = 1;
	}
	pGui->addImage("Noise Texture", noiseTex, vec2(200,200), true, false);
}

void PreProcess::ReadBuffer(const Buffer::SharedPtr & buffer, bool isIndecies)
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
	for (int j = 0; j < buffer->getSize() / 4 / 3+1; j++)
	{
		for (int i = 0; i < 3 && (j * 3 + i) < buffer->getSize() / 4; i++)
		{
			//std::cout << vPosition[j * 3 + i] << " ";
			char chInput[512];
			if (!isIndecies)
				sprintf(chInput, "%f ", vPosition[j * 3 + i]);
			else
				sprintf(chInput, "%d ", vIndecies[j * 3 + i]);

			OutputDebugStringA(chInput);
		}
		OutputDebugStringA("\n");
	}
	buffer->unmap();
}

//perlin噪声图每像素范围0-1
Texture::SharedPtr PreProcess::createPerlinNoise(uint width, uint height, float freq, float scale)
{
	std::vector<vec4> noiseData;
	noiseData.resize(width*height);

	float xFactor = 1.0f / (width - 1);
	float yFactor = 1.0f / (height - 1);

	for (uint row = 0; row < height; row++) {
		for (uint col = 0; col < width; col++) {
			float x = xFactor * col;
			float y = yFactor * row;
			float sum = 0.0f;
			float frequency = freq;
			float amplitude = scale;
			vec4 pixel(0.0f, 0.0f, 0.0f, 0.0f);
			//计算四个倍频的perlin值
			for (int oct = 0; oct < 4; oct++) {
				vec2 p(x*frequency, y*frequency);
				float sample = glm::perlin(p, vec2(frequency))/amplitude;
				sum += sample;
				float result = (sum + 1.0f) / 2.0f;  //将Perlin范围[-1,1]投影到[0,1]

				//将计算的噪声值存入像素中
				pixel[oct] = result;

				frequency *= 2.0f;
				amplitude *= scale;
			}
			//输出调试用灰度图
			//pixel = vec4(pixel.w, pixel.w, pixel.w, 1.0f);
			//范围[0,1]存入vector中
			noiseData[row*width + col] = pixel;
		}
	}
	Texture::SharedPtr noise = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1u, noiseData.data());
	return noise;
}

