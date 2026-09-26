// Copyright Winyunq, 2025. All Rights Reserved.

#include "SceneFog/FogOfWarSceneViewExtension.h"

#include "CommonRenderResources.h"
#include "DynamicRHI.h"
#include "GlobalShader.h"
#include "PipelineStateCache.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"

namespace
{
	/**
	 * 圆盘铺成外接正多边形的三角形条数，必须与 FogOfWarScene.usf 里的 VISION_SPLAT_TRIANGLES 一致。
	 * 16 条时多边形面积是圆面积的 1.013 倍：完整包住圆盘（内接多边形会把最外圈软化段切掉），
	 * 过度绘制只有 2%。
	 */
	constexpr uint32 VisionSplatTriangleCount = 16;

	/**
	 * 两趟 pass 共用的固定管线状态。
	 * 顶点位置全部由 SV_VertexID / SV_InstanceID 程序化生成，因此不需要顶点缓冲（空顶点声明）。
	 */
	void ConfigureFogOfWarPipeline(
		FRHICommandList& RHICmdList,
		FGraphicsPipelineStateInitializer& OutPipelineState,
		FRHIVertexShader* VertexShader,
		FRHIPixelShader* PixelShader)
	{
		RHICmdList.ApplyCachedRenderTargets(OutPipelineState);
		OutPipelineState.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		OutPipelineState.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		// 默认是覆盖写。散射那趟会换成 max 混合（重叠圆盘取并集），合成那趟保持覆盖写。
		OutPipelineState.BlendState = TStaticBlendState<>::GetRHI();
		OutPipelineState.PrimitiveType = PT_TriangleList;
		OutPipelineState.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
		OutPipelineState.BoundShaderState.VertexShaderRHI = VertexShader;
		OutPipelineState.BoundShaderState.PixelShaderRHI = PixelShader;
	}
}

// ---------------------------------------------------------------------------------------------
// 着色器声明
// ---------------------------------------------------------------------------------------------

/**
 * 散射 pass 的 pass 参数：只承担 RDG 的资源依赖与渲染目标绑定，不参与着色器绑定
 * （与合成趟同构：顶点/像素阶段各自用最小的参数结构，避免跨阶段字段不匹配）。
 *
 * @note 顶点/像素阶段各自的结构里保存的是同一批 RDG 资源对象的指针副本，而 RDG 是按
 *       "pass 参数结构里出现过哪些资源"来决定要创建哪些 view、并在 pass 执行前就地把这些
 *       资源对象转成 RHI 的（FRDGBuilder::ConvertToExternalTexture / InitViewRHI 改的是资源
 *       对象本身，不是结构里的字段）。所以着色器要绑定的 RDG 资源必须在这份结构里再列一次 ——
 *       这里列 VisionSources 不是冗余：少了它，那条 StructuredBuffer SRV 就永远不会被建出来。
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFogOfWarSceneSplatPassParameters, )
	/** 覆盖率遮罩（本趟输出）。 */
	RENDER_TARGET_BINDING_SLOTS()

	/** 每帧的视野源表：(WorldX, WorldY, Radius, Reserved)。 */
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VisionSources)
END_SHADER_PARAMETER_STRUCT()

/**
 * 散射顶点着色器：每条视野源一个实例，程序化生成一个外接正多边形并投影到屏幕。
 * 世界 → 裁剪用 FSceneView::ViewMatrices.GetWorldToClip()，因此不假设相机是正交、也不假设地图朝向。
 */
class FFogOfWarSceneSplatVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneSplatVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** 每帧的视野源表：(WorldX, WorldY, Radius, Reserved)。 */
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VisionSources)

		/** 世界 → 裁剪空间。 */
		SHADER_PARAMETER(FMatrix44f, WorldToClip)

		/** 视野圆所在的世界 Z 平面（cm）。 */
		SHADER_PARAMETER(float, FogPlaneZ)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * 散射像素着色器：覆盖率 = 1 - smoothstep(Radius - EdgeWidth, Radius, Distance)，圆内 1、圆外 0。
 * 旧材质里写成 1.5 - smoothstep(...) 的笔误（圆外基线被抬到 0.5、整屏雾被削掉约一半）随材质一起消失。
 *
 * @note 每个入口点引用到的全局变量都必须出现在它自己这个类的 FParameters 里：着色器编译器会按
 *       "入口点用到的名字"去根参数结构里找，找不到就编译失败（SceneFogEdgeWidth 只被像素阶段读，
 *       因此它属于这里，而不是顶点阶段的结构）。
 */
class FFogOfWarSceneSplatPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneSplatPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** 视野圆边缘软化宽度（cm）；0 = 硬边。 */
		SHADER_PARAMETER(float, SceneFogEdgeWidth)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * 合成 pass 的 pass 参数：只承担 RDG 的资源依赖与渲染目标绑定，不参与着色器绑定。
 * 这样顶点/像素两个阶段可以各自用最小的参数结构，不必强行共用一份（共用会引入
 * "某个字段只被一个阶段声明"的跨阶段绑定问题）。
 *
 * @note 这里列出的两张输入纹理同样不是冗余：像素阶段的结构里只是它们的指针副本，
 *       资源对象本身要靠在 pass 参数里出现才会被 RDG 分配并转成 RHI（理由见散射趟同名注释）。
 */
BEGIN_SHADER_PARAMETER_STRUCT(FFogOfWarSceneCompositePassParameters, )
	/** 合成输出。 */
	RENDER_TARGET_BINDING_SLOTS()

	/** 输入场景色（Tonemap 之后的 LDR 颜色）。 */
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)

	/** 视野覆盖率遮罩。 */
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageMaskTexture)
END_SHADER_PARAMETER_STRUCT()

/** 合成顶点着色器：一个覆盖整个视口的三角形，同时把 [0,1] 的视口 UV 变换到两张输入纹理各自的 UV。 */
class FFogOfWarSceneCompositeVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneCompositeVS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneCompositeVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** xy = 缩放，zw = 偏移：把视口 UV 映射到纹理 UV。 */
		SHADER_PARAMETER(FVector4f, SceneColorUVScaleBias)
		SHADER_PARAMETER(FVector4f, CoverageMaskUVScaleBias)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * 合成像素着色器：可见处用原色，不可见处压暗到 FogBrightness 倍 —— 与旧材质完全相同的合成语义。
 */
class FFogOfWarSceneCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageMaskTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, CoverageMaskSampler)
		SHADER_PARAMETER(float, SceneFogNotVisibleRegionBrightness)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneSplatVS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "VisionSplatVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneSplatPS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "VisionSplatPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneCompositeVS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "CompositeVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneCompositePS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "CompositePS", SF_Pixel);

// ---------------------------------------------------------------------------------------------
// FFogOfWarSceneViewExtension
// ---------------------------------------------------------------------------------------------

FFogOfWarSceneViewExtension::FFogOfWarSceneViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FFogOfWarSceneViewExtension::UploadFrameData_GameThread(TArray<FVector4f>& InOutSources, const FFogOfWarSceneFogSettings& InSettings)
{
	check(IsInGameThread());

	FScopeLock Lock(&PendingLock);

	// 交换而不是拷贝：交出去的数组与换回来的上一帧缓冲都留在原地复用，两侧都不会产生分配
	//（调用方拿回数组后 Reset + Add 复用它的容量即可）。
	Swap(PendingSources, InOutSources);
	PendingSettings = InSettings;
}

void FFogOfWarSceneViewExtension::SubscribeToPostProcessingPass(
	EPostProcessingPass Pass,
	const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks,
	bool bIsPassEnabled)
{
	// 只挂在 Tonemap 之后：与旧材质所在的 "After Tonemapping" 是同一个位置
	//（此时场景色已是 LDR，旧材质做的就是在 LDR 上的 lerp 压暗）。
	// bIsPassEnabled 为 false 说明这一帧的后处理序列里没有这个 pass，挂了也不会被执行。
	if (Pass != EPostProcessingPass::Tonemap || !bIsPassEnabled)
	{
		return;
	}

	// 取本帧快照。这里必须拷贝而不能把 PendingSources 换走：多视口时同一帧会 Subscribe 多次，
	// 换走会让第二个视口拿到空列表（也就丢了那一路的雾）。
	{
		FScopeLock Lock(&PendingLock);
		RenderThreadSources = PendingSources;
		RenderThreadSettings = PendingSettings;
	}

	// 没有视野源（未激活 / 本帧无采集 / 上限配成 0）就干脆不注入回调：连一次全屏 pass 都不产生。
	if (RenderThreadSources.Num() == 0)
	{
		return;
	}

	InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FFogOfWarSceneViewExtension::PostProcess_RenderThread));
}

FScreenPassTexture FFogOfWarSceneViewExtension::PostProcess_RenderThread(
	FRDGBuilder& GraphBuilder,
	const FSceneView& View,
	const FPostProcessMaterialInputs& Inputs)
{
	check(IsInRenderingThread());

	// 兜底：理论上 SubscribeToPostProcessingPass 已经过滤过空表。
	if (RenderThreadSources.Num() == 0)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	// 后处理链的输入是 slice（纹理数组视图）。非数组/非多视图时这里是零拷贝的裸纹理视图，
	// 只有真的是数组切片时才插入一次拷贝。
	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(
		GraphBuilder, Inputs.GetInput(EPostProcessMaterialInput::SceneColor));
	if (!SceneColor.IsValid())
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	// OverrideOutput 有效 = 本回调是该 pass 之后最后一个要写这张纹理的环节（引擎的
	// AcceptOverrideIfLastPass），此时必须写进它，否则后面的环节拿不到我们的结果。
	FScreenPassRenderTarget Output = Inputs.OverrideOutput;
	if (!Output.IsValid())
	{
		// 不能就地写场景色：合成阶段还要按 UV 采样它，同一张纹理在同一趟里既当 SRV 又当 RTV 是
		// 未定义行为（引擎的 PostProcessMaterial 出于同样理由另建一张中间纹理）。
		// 注意这不是边角分支：Tonemap 之后还挂着 FXAA / SMAA 时 OverrideOutput 就是无效的，
		// 而这两种 AA 恰恰是默认配置。
		FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
		OutputDesc.Reset();
		OutputDesc.ClearValue = FClearValueBinding::Black;
		Output = FScreenPassRenderTarget(
			GraphBuilder.CreateTexture(OutputDesc, TEXT("FogOfWar.SceneColor")),
			SceneColor.ViewRect,
			View.GetOverwriteLoadAction());
	}

	const FIntPoint ViewSize = Output.ViewRect.Size();
	const int32 MaskResolutionDivisor = FMath::Clamp(RenderThreadSettings.MaskResolutionDivisor, 1, 4);

	// 遮罩按"视口矩形"建立，而不是按整张输出纹理：这样无论 ViewRect 落在纹理的什么位置
	//（分屏、编辑器视口），NDC → 遮罩像素的映射都等价于 NDC → ViewRect，且与分辨率分母无关。
	const FIntPoint MaskExtent(
		FMath::Max(1, FMath::DivideAndRoundUp(ViewSize.X, MaskResolutionDivisor)),
		FMath::Max(1, FMath::DivideAndRoundUp(ViewSize.Y, MaskResolutionDivisor)));

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFogOfWarSceneSplatVS> SplatVertexShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneSplatPS> SplatPixelShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneCompositeVS> CompositeVertexShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneCompositePS> CompositePixelShader(ShaderMap);

	// ---- 趟 1：散射。每条视野源一个实例化外接多边形，写入覆盖率遮罩 ----
	// 源数据每帧上传一次。QueueBufferUpload 内部会自己拷贝，因此这里的源数组不要求活到执行期。
	FRDGBufferRef VisionSourceBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), RenderThreadSources.Num()),
		TEXT("FogOfWar.VisionSources"));
	GraphBuilder.QueueBufferUpload(
		VisionSourceBuffer,
		RenderThreadSources.GetData(),
		RenderThreadSources.Num() * sizeof(FVector4f),
		ERDGInitialDataFlags::None);

	// 单通道 8 位：软化边缘只有 0..1 的精度需求，8 位足够，带宽与占用最小。
	FRDGTextureRef CoverageMaskTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(MaskExtent, PF_R8, FClearValueBinding::Black, TexCreate_RenderTargetable | TexCreate_ShaderResource),
		TEXT("FogOfWar.CoverageMask"));

	auto* SplatPassParameters = GraphBuilder.AllocParameters<FFogOfWarSceneSplatPassParameters>();
	SplatPassParameters->RenderTargets[0] = FRenderTargetBinding(CoverageMaskTexture, ERenderTargetLoadAction::EClear);
	SplatPassParameters->VisionSources = GraphBuilder.CreateSRV(VisionSourceBuffer);

	FFogOfWarSceneSplatVS::FParameters SplatVSParameters;
	SplatVSParameters.VisionSources = SplatPassParameters->VisionSources;
	SplatVSParameters.WorldToClip = FMatrix44f(View.ViewMatrices.GetWorldToClip());
	SplatVSParameters.FogPlaneZ = RenderThreadSettings.PlaneZ;

	FFogOfWarSceneSplatPS::FParameters SplatPSParameters;
	SplatPSParameters.SceneFogEdgeWidth = FMath::Max(RenderThreadSettings.EdgeWidthCm, 0.0f);

	const uint32 SourceCount = static_cast<uint32>(RenderThreadSources.Num());

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("FogOfWar.VisionSplat(%u)", SourceCount),
		SplatPassParameters,
		ERDGPassFlags::Raster,
		[SplatPassParameters, SplatVSParameters, SplatPSParameters, SplatVertexShader, SplatPixelShader, SourceCount, MaskExtent](FRDGAsyncTask, FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(0.0f, 0.0f, 0.0f, static_cast<float>(MaskExtent.X), static_cast<float>(MaskExtent.Y), 1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			ConfigureFogOfWarPipeline(RHICmdList, GraphicsPSOInit, SplatVertexShader.GetVertexShader(), SplatPixelShader.GetPixelShader());
			// 重叠圆盘必须取并集：圆盘外沿是软化段，若用覆盖写，后画的圆会在先画的圆上切出一道缺口。
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RED, BO_Max, BF_One, BF_One, BO_Max, BF_One, BF_One>::GetRHI();
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, SplatVertexShader, SplatVertexShader.GetVertexShader(), SplatVSParameters);
			SetShaderParameters(RHICmdList, SplatPixelShader, SplatPixelShader.GetPixelShader(), SplatPSParameters);

			// 顶点完全程序化生成，不需要流源。NumPrimitives = 每条实例的三角形数，NumInstances = 源数。
			RHICmdList.SetStreamSource(0, nullptr, 0);
			RHICmdList.DrawPrimitive(0, VisionSplatTriangleCount, SourceCount);
		});

	// ---- 趟 2：合成。一个覆盖整个视口的三角形，把遮罩合到场景色上 ----
	const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

	auto* CompositePassParameters = GraphBuilder.AllocParameters<FFogOfWarSceneCompositePassParameters>();
	CompositePassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	CompositePassParameters->SceneColorTexture = SceneColor.Texture;
	CompositePassParameters->CoverageMaskTexture = CoverageMaskTexture;

	FFogOfWarSceneCompositeVS::FParameters CompositeVSParameters;
	CompositeVSParameters.SceneColorUVScaleBias = FVector4f(
		static_cast<float>(SceneColor.ViewRect.Width()) / static_cast<float>(SceneColorExtent.X),
		static_cast<float>(SceneColor.ViewRect.Height()) / static_cast<float>(SceneColorExtent.Y),
		static_cast<float>(SceneColor.ViewRect.Min.X) / static_cast<float>(SceneColorExtent.X),
		static_cast<float>(SceneColor.ViewRect.Min.Y) / static_cast<float>(SceneColorExtent.Y));
	// 遮罩正好覆盖视口矩形，这里只需按"向上取整后的遮罩尺寸"做一次亚像素校正。
	CompositeVSParameters.CoverageMaskUVScaleBias = FVector4f(
		static_cast<float>(ViewSize.X) / static_cast<float>(MaskExtent.X * MaskResolutionDivisor),
		static_cast<float>(ViewSize.Y) / static_cast<float>(MaskExtent.Y * MaskResolutionDivisor),
		0.0f,
		0.0f);

	FFogOfWarSceneCompositePS::FParameters CompositePSParameters;
	CompositePSParameters.SceneColorTexture = SceneColor.Texture;
	CompositePSParameters.SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	CompositePSParameters.CoverageMaskTexture = CoverageMaskTexture;
	CompositePSParameters.CoverageMaskSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	CompositePSParameters.SceneFogNotVisibleRegionBrightness = RenderThreadSettings.NotVisibleRegionBrightness;

	const FIntRect OutputRect = Output.ViewRect;

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("FogOfWar.Composite"),
		CompositePassParameters,
		ERDGPassFlags::Raster,
		[CompositePassParameters, CompositeVSParameters, CompositePSParameters, CompositeVertexShader, CompositePixelShader, OutputRect](FRDGAsyncTask, FRHICommandList& RHICmdList)
		{
			RHICmdList.SetViewport(
				static_cast<float>(OutputRect.Min.X),
				static_cast<float>(OutputRect.Min.Y),
				0.0f,
				static_cast<float>(OutputRect.Max.X),
				static_cast<float>(OutputRect.Max.Y),
				1.0f);

			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			ConfigureFogOfWarPipeline(RHICmdList, GraphicsPSOInit, CompositeVertexShader.GetVertexShader(), CompositePixelShader.GetPixelShader());
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, CompositeVertexShader, CompositeVertexShader.GetVertexShader(), CompositeVSParameters);
			SetShaderParameters(RHICmdList, CompositePixelShader, CompositePixelShader.GetPixelShader(), CompositePSParameters);

			// 1 条三角形覆盖整个视口，顶点程序化生成。
			RHICmdList.SetStreamSource(0, nullptr, 0);
			RHICmdList.DrawPrimitive(0, 1, 1);
		});

	return Output;
}
