#pragma once
#include "../SharedUtils/RenderPass.h"
#include "../SharedUtils/RasterLaunch.h"
#include "../SharedUtils/SimpleVars.h"
#include "../SharedUtils/RayLaunch.h"

class PreProcess : public ::RenderPass, inherit_shared_from_this<::RenderPass, PreProcess>
{
public:
	using SharePtr = std::shared_ptr<PreProcess>;

	static SharedPtr create() { return SharePtr(new PreProcess()); }
	virtual ~PreProcess() = default;

protected:
	PreProcess(): ::RenderPass("Pre-Processing Pass", "Pre-Processing Pass Options"){}

	// Implementation of RenderPass interface
	bool initialize(RenderContext* pRenderContext, ResourceManager::SharedPtr pResManager) override;
	void execute(RenderContext* pRenderContext) override;
	void initScene(RenderContext* pRenderContext, Scene::SharedPtr pScene) override;
	void renderGui(Gui* pGui) override;

	bool requiresScene() override { return true; }
	bool usesRasterization() override { return true; }
	bool usesRayTracing() override { return true; }

	// Internal pass state
	RayLaunch::SharedPtr        mpRays;            ///< Our wrapper around a DX Raytracing pass
	RtScene::SharedPtr          mpRtScene;           ///<  A copy of our scene
	Scene::SharedPtr            mpScene;
	RasterLaunch::SharedPtr     mpRaster;               ///< A wrapper managing the shader for our g-buffer creation
	GraphicsState::SharedPtr    mpGfxState;             ///< Our graphics pipeline state (i.e., culling, raster, blend settings)

	// What's our background color?
	vec3                        mBgColor = vec3(0.5f, 0.5f, 1.0f);
	Buffer::SharedPtr ExposeVertex;
	Buffer::SharedPtr ChangeVertexBuffer;
	Buffer::SharedPtr ChangeVertexNormal;
	Fbo::SharedPtr outputFbo;
	
	//预处理标志位，0表示未处理，1表示在处理，>1表示已处理
	uint32_t ProcessState = 0;
	Texture::SharedPtr noiseTex;
	Sampler::SharedPtr mpNoiseSampler;

	//Debug Function
	void ReadBuffer(const Buffer::SharedPtr& buffer, bool isIndecies = false);
	Texture::SharedPtr createPerlinNoise(uint width, uint height, float freq, float scale);
};

