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

	//����Ĭ�ϳ���
	mpResManager->setDefaultSceneName("Data/cube/cube.fscene");

	// ���ù���׷��shader
	mpRays = RayLaunch::create(kFileRayTrace, kEntryPointRayGen);
	mpRays->addMissShader(kFileRayTrace, kEntryPointMiss0);                             // Add miss shader #0 
	mpRays->addHitShader(kFileRayTrace, kEntryPrimaryClosestHit, kEntryPrimaryAnyHit);  // Add hit group #0
	// Now that we've passed all our shaders in, compile.  If we already have our scene, let it know what scene to use.
	mpRays->compileRayProgram();
	if (mpRtScene) mpRays->setScene(mpRtScene);

	//���ù�դ��shader
	mpResManager->requestTextureResources({ "WorldPosition", "WorldNormal", "MaterialDiffuse",
											"MaterialSpecRough", "MaterialExtraParams","Prediction" });

	// We also need a depth buffer to use when rendering our g-buffer.  Ask for one, with appropriate format and binding flags.
	mpResManager->requestTextureResource("Z-Buffer", ResourceFormat::D24UnormS8, ResourceManager::kDepthBufferFlags);

	

	// Since we're rasterizing, we need to define our raster pipeline state (though we use the defaults)
	mpGfxState = GraphicsState::create();

	// Create our wrapper for a scene-rasterization pass.
	mpRaster = RasterLaunch::createFromFiles(kGbufVertShader, kGbufFragShader);
	mpRaster->setScene(mpScene);

	//��������ͼ������������
	noiseTex = createPerlinNoise(200, 200, 20.0f, 2.0f);
	Sampler::Desc samplerDesc;
	samplerDesc.setFilterMode(Sampler::Filter::Linear, Sampler::Filter::Linear, Sampler::Filter::Linear).setAddressingMode(Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap, Sampler::AddressMode::Wrap);
	mpNoiseSampler = Sampler::create(samplerDesc);

	//��������ͼ
	mpResManager->manageTextureResource("Noise", noiseTex);

	return true;
}

void PreProcess::execute(RenderContext * pRenderContext)
{
	// Check that we're ready to render
	if (!mpRays || !mpRays->readyToRender()) return;

	//ִ�л�ѩԤ����
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

		//ִ�ж���λ�Ʋ���
		//��һ����λ�ƺ�Ķ���λ�ø��Ƶ�ԭ���Ķ��������ڣ�����RtScene�ļ��ٽṹ������Ҫ���¹�����

		pContext->copyResource(mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get(), ChangeVertexBuffer.get());
		pContext->flush(true);

		//���¼��ٽṹ
		dynamic_cast<RtModel*>(mpRtScene->getModel(0).get())->updateBottomLevelData();
		ProcessState++;
		//ReadBuffer(ExposeVertex, false);
		//ReadBuffer(ChangeVertexBuffer, false);
	}

	//debug���ɵ�����ͼ
	// Get a pointer to a Falcor texture resource for our output 
	/*Texture::SharedPtr outTex = mpResManager->getTexture(ResourceManager::kOutputChannel);
	if (!outTex) return;
	Texture::SharedPtr inTex = createPerlinNoise(200, 200, 15.0f, 2.0f);
	pRenderContext->blit(inTex->getSRV(), outTex->getRTV(),uvec4(-1),uvec4(800,400,1000,600));*/

	//����GBuffer
	// Create a framebuffer for rendering.  (Creating once per frame is for simplicity, not performance).
	outputFbo = mpResManager->createManagedFbo(
		{ "WorldPosition", "WorldNormal", "MaterialDiffuse", "MaterialSpecRough", "MaterialExtraParams", "Prediction" }, // Names of color buffers
		"Z-Buffer");                                                                                      // Name of depth buffer
	if (!outputFbo) return;
	// Clear g-buffer.  Clear colors to black, depth to 1, stencil to 0, but then clear diffuse texture to our bg color
	pRenderContext->clearFbo(outputFbo.get(), vec4(0, 0, 0, 0), 1.0f, 0);
	pRenderContext->clearUAV(outputFbo->getColorTexture(2)->getUAV().get(), vec4(mBgColor, 1.0f));

	//����rasterization���߱���
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

	//����Expose���棬Ĭ��Ϊ0
	std::vector<float> temp;
	temp.resize(mpRtScene->getModel(0)->getVertexCount());
	std::generate(temp.begin(), temp.end(), [] {return 0.0f; });
	ExposeVertex = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float), Resource::BindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None, temp.data());
	auto HitVar = mpRays->getGlobalVars();
	bool vertifyExpose = HitVar->setRawBuffer("ExposeVertex", ExposeVertex);

	//����ƫ�ƺ�Ķ���buffer
	ChangeVertexBuffer = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	RenderContext* pContext = gpDevice->getRenderContext().get();
	pContext->copyResource(ChangeVertexBuffer.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get());
	pContext->flush(true);

	//��������ķ�����buffer
	ChangeVertexNormal = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	pContext->copyResource(ChangeVertexNormal.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(1).get());
	pContext->flush(true);

	//����ԭʼ�Ķ���buffer
	OriginVertexBuffer = Buffer::create(mpRtScene->getModel(0)->getVertexCount() * sizeof(float3), Resource::BindFlags::ShaderResource | Resource::BindFlags::UnorderedAccess, Buffer::CpuAccess::None);
	pContext->copyResource(OriginVertexBuffer.get(), mpRtScene->getModel(0)->getMesh(0)->getVao()->getVertexBuffer(0).get());
	pContext->flush(true);

	//�����ã���֤�Ƿ��Ѿ����óɹ�
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

		//���¼��ٽṹ
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

//perlin����ͼÿ���ط�Χ0-1
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
			//�����ĸ���Ƶ��perlinֵ
			for (int oct = 0; oct < 4; oct++) {
				vec2 p(x*frequency, y*frequency);
				float sample = glm::perlin(p, vec2(frequency))/amplitude;
				sum += sample;
				float result = (sum + 1.0f) / 2.0f;  //��Perlin��Χ[-1,1]ͶӰ��[0,1]

				//�����������ֵ����������
				pixel[oct] = result;

				frequency *= 2.0f;
				amplitude *= scale;
			}
			//��������ûҶ�ͼ
			//pixel = vec4(pixel.w, pixel.w, pixel.w, 1.0f);
			//��Χ[0,1]����vector��
			noiseData[row*width + col] = pixel;
		}
	}
	Texture::SharedPtr noise = Texture::create2D(width, height, ResourceFormat::RGBA32Float, 1, 1u, noiseData.data());
	return noise;
}

