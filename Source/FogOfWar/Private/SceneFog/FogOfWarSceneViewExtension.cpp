// Copyright Winyunq, 2025. All Rights Reserved.

#include "SceneFog/FogOfWarSceneViewExtension.h"

#include "CommonRenderResources.h"
#include "DynamicRHI.h"
// UTextureRenderTarget2D：视野场的持久载体（打包趟直接写进它）。
#include "Engine/TextureRenderTarget2D.h"
#include "FogOfWar.h"
#include "GlobalShader.h"
// CreateRenderTarget：把 UTextureRenderTarget2D 的 RHI 纹理包装成 RDG 可注册的外部纹理。
#include "PooledRenderTarget.h"
#include "Math/IntVector.h"
#include "PipelineStateCache.h"
#include "PixelShaderUtils.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIStaticStates.h"
#include "ScreenPass.h"
#include "ShaderParameterStruct.h"
#include "SceneRenderTargetParameters.h" // 场景纹理相关的公共定义（ESceneTextureSetupMode 等）
#include "SceneTexturesConfig.h"         // FSceneTextureUniformParameters：合成趟读场景深度用的 uniform buffer

namespace
{
	/**
	 * 场内覆盖率的定标分母：场纹理用整数做原子取最大，落回浮点时除以它。
	 * 8 位与最终 PF_R8G8 的通道精度一致，再高的定标只会在打包时被丢掉。
	 */
	constexpr float VisionCoverageScale = 255.0f;

	/**
	 * 合成趟共用的固定管线状态。
	 * 顶点位置全部由 SV_VertexID 程序化生成，因此不需要顶点缓冲（空顶点声明）。
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
		OutPipelineState.BlendState = TStaticBlendState<>::GetRHI();
		OutPipelineState.PrimitiveType = PT_TriangleList;
		OutPipelineState.BoundShaderState.VertexDeclarationRHI = GEmptyVertexDeclaration.VertexDeclarationRHI;
		OutPipelineState.BoundShaderState.VertexShaderRHI = VertexShader;
		OutPipelineState.BoundShaderState.PixelShaderRHI = PixelShader;
	}

}

// ---------------------------------------------------------------------------------------------
// 着色器声明
//
// 趟 1（散射）与趟 2（打包）各自只有一个阶段，参数结构可以就地定义；趟 3（合成）有顶点/像素
// 两个阶段，必须把"pass 参数（RDG 依赖 + 渲染目标）"与"各阶段最小参数"分开 —— 共用一份会让
// 某个字段只被一个阶段声明，而着色器编译器是按"入口点用到的名字"去根参数结构里找的。
//
// @note 顶点/像素阶段的结构里保存的是同一批 RDG 资源对象的指针副本，而 RDG 是按"pass 参数结构里
//       出现过哪些资源"来决定要创建哪些 view、并在 pass 执行前就地把这些资源对象转成 RHI 的。
//       所以着色器要绑定的 RDG 资源必须在这份 pass 结构里再列一次 —— 这不是冗余：少了它，
//       那条 SRV/UAV 就永远不会被建出来。
// ---------------------------------------------------------------------------------------------

/**
 * 散射 compute：每条视野源只在自己的场 AABB 内派发线程，把定标覆盖率用 InterlockedMax 写进整数场。
 * 这是"彻底消掉重叠 overdraw"的落点 —— 重叠区只剩整数原子比较，不再有逐层像素着色与混合。
 */
class FFogOfWarSceneVisionFieldSplatCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneVisionFieldSplatCS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneVisionFieldSplatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** 每帧的视野源表：(WorldX, WorldY, Radius, Reserved)。 */
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VisionSources)

		// 与 VisionSources 同索引的扇形参数 (dirX, dirY, cosHalfAngle, 0)。
		// cosHalfAngle >= 1 表示该源是全向的（CPU 对张角 >= 360 的源写 2.0），着色器据此跳过扇形判定。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, VisionSourceDirs)

		/** 每条源的场 AABB：xy = 最小纹素（含），zw = 最大纹素（不含）。 */
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, VisionSourceAabbs)

		/** 覆盖率场（定标 0..255 的整数）。 */
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, VisionCoverage)

		/** 场纹素 (0,0) 左上角的世界 XY。 */
		SHADER_PARAMETER(FVector2f, FieldWorldMin)

		/** 单个场纹素的世界边长（cm）。 */
		SHADER_PARAMETER(float, FieldTexelSizeCm)

		/** 视野圆边缘软化宽度（cm）；0 = 硬边。 */
		SHADER_PARAMETER(float, SceneFogEdgeWidth)

		/** 覆盖率定标分母（255）。 */
		SHADER_PARAMETER(float, VisionCoverageScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * 打包像素着色器：R = 当前覆盖率，G = 已探索。全屏顶点由 FPixelShaderUtils 提供（不需要自备 VS），
 * 而那个入口要求参数结构自己带上渲染目标绑定 —— 因此这里把 RENDER_TARGET_BINDING_SLOTS 直接放进
 * PS 的 FParameters，不再另拆一份 pass 参数：本趟只有像素一个阶段，没有"某个字段只被一个阶段声明"
 * 的问题（那才是合成趟必须拆参数的原因）。
 */
class FFogOfWarSceneVisionFieldPackPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneVisionFieldPackPS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneVisionFieldPackPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** 视野场（本趟输出，PF_R8G8）。 */
		RENDER_TARGET_BINDING_SLOTS()

		/** 趟 1 的整数覆盖率。 */
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, VisionCoverageTexture)

		/** 历史已探索层（LSB-first 位流）。 */
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VisionExploredBits)

		/** 已探索位图的分辨率（格数）；场与它共用同一个世界矩形，UV 直接换算。 */
		SHADER_PARAMETER(FIntPoint, VisionExploredExtent)

		/** 场分辨率（纹素数）。 */
		SHADER_PARAMETER(FIntPoint, FieldTexelCount)

		SHADER_PARAMETER(float, VisionCoverageScale)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/** 合成 pass 的 pass 参数：只承担 RDG 的资源依赖与渲染目标绑定，不参与着色器绑定。 */
BEGIN_SHADER_PARAMETER_STRUCT(FFogOfWarSceneCompositePassParameters, )
	/** 合成输出。 */
	RENDER_TARGET_BINDING_SLOTS()

	/** 输入场景色（Tonemap 之后的 LDR 颜色）。 */
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)

	/** 覆盖率 + 已探索打包成的 R8G8。 */
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisionFieldTexture)

	/** 场景纹理 uniform buffer（含场景深度）：像素反投影用；RDG 依赖由这份 pass 结构登记。 */
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
END_SHADER_PARAMETER_STRUCT()

/**
 * 合成顶点着色器：一个覆盖整个视口的三角形，输出视口 UV 与换算好的场景色 UV。
 *
 * @note 反投影（视口 UV → 世界 XY）刻意不在这里做。顶点输出 Position.w = 1，光栅器据此判定
 *       “无透视”并对所有附加量做屏幕空间线性插值，而世界平面上的 XY 是屏幕坐标的有理函数
 *       （含透视除法）：只有三角形三个角上正确，中间会被拉偏，偏移量还随俯仰角变化 ——
 *       症状就是雾随相机转动而相对场景滑移。视口 UV 与屏幕像素是线性关系，才是能安全插值的量。
 */
class FFogOfWarSceneCompositeVS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneCompositeVS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneCompositeVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** xy = 缩放，zw = 偏移：把视口 UV 映射到场景色纹理的 UV。 */
		SHADER_PARAMETER(FVector4f, SceneColorUVScaleBias)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

/**
 * 合成像素着色器：逐像素反投影到世界平面，采样 R8G8，三态合成 ——
 * 从未探索 / 已探索但当前不可见 / 当前可见。
 *
 * @note 同一个字段（如 InvViewProjection）在顶点与像素两个阶段都要用时，必须出现在真正引用它的
 *       那个阶段的 FParameters 里。这里 WS/PS 各只有一处引用，因此各放一份，不做共用结构。
 */
class FFogOfWarSceneCompositePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFogOfWarSceneCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FFogOfWarSceneCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		/** 裁剪空间 → 世界（FViewMatrices::GetClipToWorld()）。 */
		SHADER_PARAMETER(FMatrix44f, InvViewProjection)

		/** 视野场所在的世界 Z 平面（cm）。 */
		SHADER_PARAMETER(float, FogPlaneZ)

		/** 场的世界最小角（XY，cm）。 */
		SHADER_PARAMETER(FVector2f, FieldWorldMin)

		/** 场的世界尺寸（XY，cm）。 */
		SHADER_PARAMETER(FVector2f, FieldWorldExtent)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SceneColorSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisionFieldTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, VisionFieldSampler)

		/**
		 * 场景纹理 uniform buffer（引擎约定名 SceneTexturesStruct，usf 侧同名访问）。
		 * 合成趟从它的 SceneDepthTexture 读 device z，把像素反投影回它真实所在的世界位置，
		 * 而不是投到固定 Z 平面 —— 否则斜视 / 地形起伏时雾会相对场景滑移。
		 * 非 deferred / 未提供时该成员为空，着色器保持 DeviceZ = 0 并退回平面投影。
		 */
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER(float, SceneFogNotVisibleRegionBrightness)
		SHADER_PARAMETER(float, SceneFogExploredRegionBrightness)

		/** 雾层自身的不透明度：0 = 完全透出场景色，1 = 完全不透明（按上面的亮度压暗）。 */
		SHADER_PARAMETER(float, SceneFogOpacity)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneVisionFieldSplatCS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "VisionFieldSplatCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneVisionFieldPackPS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "VisionFieldPackPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneCompositeVS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "CompositeVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FFogOfWarSceneCompositePS, "/Plugin/FogOfWar/Private/FogOfWarScene.usf", "CompositePS", SF_Pixel);

// ---------------------------------------------------------------------------------------------
// FFogOfWarSceneViewExtension
// ---------------------------------------------------------------------------------------------

FFogOfWarSceneViewExtension::FFogOfWarSceneViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FFogOfWarSceneViewExtension::UploadFrameData_GameThread(
	TArray<FVector4f>& InOutSources,
	TArray<FVector4f>& InOutSourceDirs,
	const FFogOfWarSceneFogSettings& InSettings,
	const FFogOfWarSceneVisionField& InField,
	bool bInSceneFogActive,
	UTextureRenderTarget2D* InFieldTexture)
{
	check(IsInGameThread());

	FScopeLock Lock(&PendingLock);

	// 交换而不是拷贝：交出去的数组与换回来的上一帧缓冲都留在原地复用，两侧都不会产生分配
	//（调用方拿回数组后 Reset + Add 复用它的容量即可）。
	Swap(PendingSources, InOutSources);
	// 扇形参数与视野源必须**同帧同索引**：一起交换，绝不允许只更新其中一条 —— 否则渲染线程会
	// 拿这一帧的朝向去裁剪上一帧的源（画面表现为扇形的朝向随机漂移）。
	Swap(PendingSourceDirs, InOutSourceDirs);
	PendingSettings = InSettings;
	PendingField = InField;
	PendingSceneFogActive = bInSceneFogActive;
	PendingFieldTexture = InFieldTexture;
}

void FFogOfWarSceneViewExtension::UploadExploredLayer_GameThread(const FFogOfWarSceneExploredLayer& InExploredLayer)
{
	check(IsInGameThread());

	FScopeLock Lock(&PendingLock);
	PendingExploredLayer = InExploredLayer;
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
		RenderThreadSourceDirs = PendingSourceDirs;  // 与 RenderThreadSources 同帧、同索引
		RenderThreadSettings = PendingSettings;
		RenderThreadField = PendingField;
		RenderThreadSceneFogActive = PendingSceneFogActive;
		RenderThreadFieldTexture = PendingFieldTexture;

		// 已探索层只在内容真的变化时拷贝：它是逐格位图，比视野源大两三个数量级，
		// 而探索层每秒只变几次。同一帧的第二个视口因为版本号已经对齐，同样会跳过。
		if (PendingExploredLayer.Version != RenderThreadExploredLayer.Version)
		{
			RenderThreadExploredLayer = PendingExploredLayer;
		}
	}

	// 不该遮蔽（雾系统关闭 / 子系统不可用）、或场几何还没就绪（地图范围未知）就干脆不注入回调：
	// 连一次全屏 pass 都不产生。
	//
	// ⚠ 这里刻意**不**判断"有没有视野源"：本帧源为空（相机移出所有单位的视野范围、或视野源被视图
	// 剔除干净）恰恰是"什么都看不见"的情形，必须跑一趟把整屏按"从未探索"遮蔽掉；早退会让迷雾
	// 在画面里凭空消失。
	if (!RenderThreadSceneFogActive || !RenderThreadField.IsValid())
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

	// 兜底：理论上 SubscribeToPostProcessingPass 已经过滤过这两种情况。
	// 同样刻意不判断"有没有视野源"：源为空时仍要走完三趟，把整屏按"从未探索"遮蔽掉。
	if (!RenderThreadSceneFogActive || !RenderThreadField.IsValid())
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

	const FFogOfWarSceneVisionField& Field = RenderThreadField;
	const FIntPoint FieldExtent = Field.TexelCount;
	const FVector2f FieldWorldMin(static_cast<float>(Field.WorldMin.X), static_cast<float>(Field.WorldMin.Y));
	const FVector2f FieldWorldExtent(static_cast<float>(Field.WorldSize.X), static_cast<float>(Field.WorldSize.Y));

	const FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());
	TShaderMapRef<FFogOfWarSceneVisionFieldSplatCS> SplatShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneVisionFieldPackPS> PackPixelShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneCompositeVS> CompositeVertexShader(ShaderMap);
	TShaderMapRef<FFogOfWarSceneCompositePS> CompositePixelShader(ShaderMap);

	const int32 SourceCount = RenderThreadSources.Num();

	// ---- 趟 1：散射。每条视野源一个线程组，只扫自己的 AABB，整数原子取最大 ----
	// R32_UINT：这是唯一能做 InterlockedMax 的场格式；8 位定标与最终 R8G8 的通道精度对齐。
	FRDGTextureRef CoverageTexture = GraphBuilder.CreateTexture(
		FRDGTextureDesc::Create2D(
			FieldExtent, PF_R32_UINT, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV),
		TEXT("FogOfWar.VisionCoverage"));

	FRDGTextureUAVRef CoverageUAV = GraphBuilder.CreateUAV(CoverageTexture);
	// 原子累加的起点必须是 0（未覆盖）。CreateTexture 的内容是未定义的，不能省这一步。
	// 本帧一条视野源都没有时（SourceCount == 0），清零后的场就是最终结果：覆盖率全 0 —— 合成趟据此
	// 把整屏判成"从未探索"，也就是**全遮蔽**。这正是"什么都看不见"应有的画面，因此下面把整个
	// 上传 + 散射趟整体跳过（0 条源既没有 buffer 可建，也没有线程组可派发），而不是让雾消失。
	AddClearUAVPass(GraphBuilder, CoverageUAV, 0u);

	if (SourceCount > 0)
	{
		// ---- 数据上传：视野源 + 每条源的场 AABB（纯算术，不含任何相机投影）----
		FRDGBufferRef VisionSourceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), SourceCount),
			TEXT("FogOfWar.VisionSources"));
		GraphBuilder.QueueBufferUpload(
			VisionSourceBuffer,
			RenderThreadSources.GetData(),
			SourceCount * sizeof(FVector4f),
			ERDGInitialDataFlags::None);

		// AABB 是"圆心 ± 半径"换算出的纹素矩形，并在两轴分别 clamp 到场范围。
		// 完全落在场外的源得到空矩形（min == max），散射 compute 里对应的循环体自然不执行。
		TArray<FIntVector4> SourceAabbs;
		SourceAabbs.SetNumUninitialized(SourceCount);
		{
			const float InvTexelSizeCm = 1.0f / Field.TexelSizeCm;
			const float WorldMinX = static_cast<float>(Field.WorldMin.X);
			const float WorldMinY = static_cast<float>(Field.WorldMin.Y);

			for (int32 Index = 0; Index < SourceCount; ++Index)
			{
				const FVector4f& Source = RenderThreadSources[Index];
				const float Radius = FMath::Max(Source.Z, 0.0f);

				const int32 MinX = FMath::Clamp(FMath::FloorToInt((Source.X - Radius - WorldMinX) * InvTexelSizeCm), 0, FieldExtent.X);
				const int32 MinY = FMath::Clamp(FMath::FloorToInt((Source.Y - Radius - WorldMinY) * InvTexelSizeCm), 0, FieldExtent.Y);
				const int32 MaxX = FMath::Clamp(FMath::CeilToInt((Source.X + Radius - WorldMinX) * InvTexelSizeCm), 0, FieldExtent.X);
				const int32 MaxY = FMath::Clamp(FMath::CeilToInt((Source.Y + Radius - WorldMinY) * InvTexelSizeCm), 0, FieldExtent.Y);

				SourceAabbs[Index] = FIntVector4(MinX, MinY, MaxX, MaxY);
			}
		}

		FRDGBufferRef AabbBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector4), SourceCount),
			TEXT("FogOfWar.VisionSourceAabbs"));
		GraphBuilder.QueueBufferUpload(
			AabbBuffer,
			SourceAabbs.GetData(),
			SourceCount * sizeof(FIntVector4),
			ERDGInitialDataFlags::None);

		auto* SplatParameters = GraphBuilder.AllocParameters<FFogOfWarSceneVisionFieldSplatCS::FParameters>();
		SplatParameters->VisionSources = GraphBuilder.CreateSRV(VisionSourceBuffer);
		SplatParameters->VisionSourceAabbs = GraphBuilder.CreateSRV(AabbBuffer);

		// 扇形参数缓冲：(dirX, dirY, cosHalfAngle, 0)，与 VisionSources 同索引、同长度。
		// 与视野源同帧上传 —— 数量上二者恒等（CPU 侧成对增删），因此这里不做长度校验。
		FRDGBufferRef VisionSourceDirBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), SourceCount),
			TEXT("FogOfWar.VisionSourceDirs"));
		GraphBuilder.QueueBufferUpload(
			VisionSourceDirBuffer,
			RenderThreadSourceDirs.GetData(),
			SourceCount * sizeof(FVector4f),
			ERDGInitialDataFlags::None);
		SplatParameters->VisionSourceDirs = GraphBuilder.CreateSRV(VisionSourceDirBuffer);
		SplatParameters->VisionCoverage = CoverageUAV;
		SplatParameters->FieldWorldMin = FieldWorldMin;
		SplatParameters->FieldTexelSizeCm = Field.TexelSizeCm;
		SplatParameters->SceneFogEdgeWidth = FMath::Max(RenderThreadSettings.EdgeWidthCm, 0.0f);
		SplatParameters->VisionCoverageScale = VisionCoverageScale;

		// GroupCount 的 Z 维就是源数：一个线程组 = 一条视野源。组内 8×8 的线程按步长扫过该源的 AABB，
		// 因此派发出来的线程全部落在"这条源真正可能覆盖的纹素"上，没有全屏 × 全源的乘积项。
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("FogOfWar.VisionFieldSplat(%d)", SourceCount),
			SplatShader,
			SplatParameters,
			FIntVector(1, 1, SourceCount));
	}

	// ---- 历史已探索层：保持位打包，只有几 KB ----
	// 位序与 FRlSpatialMap::ExploredBitmap 一致，所以从不展开；没有提供者时给一个 0 元素的兜底，
	// 让着色器里的 G 通道恒为 0（画面退化成阶段一的二态雾）。
	FIntPoint ExploredExtent(1, 1);
	FRDGBufferRef ExploredBitsBuffer = nullptr;
	if (RenderThreadExploredLayer.IsValid())
	{
		ExploredExtent = RenderThreadExploredLayer.Extent;
		ExploredBitsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), RenderThreadExploredLayer.PackedBits.Num()),
			TEXT("FogOfWar.ExploredBits"));
		GraphBuilder.QueueBufferUpload(
			ExploredBitsBuffer,
			RenderThreadExploredLayer.PackedBits.GetData(),
			RenderThreadExploredLayer.PackedBits.Num() * sizeof(uint32),
			ERDGInitialDataFlags::None);
	}
	else
	{
		static const uint32 EmptyExploredBits = 0u;
		ExploredBitsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("FogOfWar.ExploredBitsEmpty"));
		GraphBuilder.QueueBufferUpload(
			ExploredBitsBuffer,
			&EmptyExploredBits,
			sizeof(uint32),
			ERDGInitialDataFlags::None);
	}

	FRDGBufferSRVRef ExploredBitsSRV = GraphBuilder.CreateSRV(ExploredBitsBuffer);

	// ---- 趟 2：打包。整数覆盖率 + 已探索 → PF_R8G8（R = 当前覆盖率，G = 已探索）----
	// 两通道放进同一张纹理，合成趟一次采样就能同时拿到"现在能不能看见"与"以前有没有看见过"。
	//
	// 输出目标有两种，取决于"本帧要不要把场对外发布"（多视口时只有第一个视口发布，见
	// LastFieldPublishFamily 的说明）：
	//   · 发布（正常情况）：**直接写进 Actor 持有的持久 RT** —— 屏幕上的雾与"迷雾遮蔽敌方单位"
	//     因此用的是同一份像素数据，遮蔽边界与雾边界逐像素重合，而且不是拷贝（零额外带宽）；
	//   · 不发布（同帧的第二个视口 / 没有可用的 RT）：退回一张瞬态场，雾照画，只是不对外发布。
	FRDGTextureRef VisionFieldTexture = nullptr;
	{
		bool bPublishToExternal = false;
		if (UTextureRenderTarget2D* FieldRT = RenderThreadFieldTexture.Get())
		{
			const FSceneViewFamily* Family = View.Family;
			const uint64 FrameNumber = Family ? Family->FrameNumber : 0;
			// 同一个 (Family, Frame) 只发布一次：多视口共用这一张外部纹理，写两次就是同一张
			// RDG 纹理上的无序写。
			const bool bAlreadyPublishedThisFrame =
				(LastFieldPublishFamily == Family) && (LastFieldPublishFrameNumber == FrameNumber);

			if (!bAlreadyPublishedThisFrame)
			{
				if (FTextureRenderTargetResource* RTResource = FieldRT->GetRenderTargetResource())
				{
					// CreateRenderTarget + RegisterExternalTexture：把 UObject 侧已有的 RHI 纹理交给 RDG
					// 管理状态转换。这正是引擎自己往 UTextureRenderTarget2D 里写数据的写法
					//（FTextureRenderTarget2DResource::UpdateDeferredResource 里就是这一句）。
					VisionFieldTexture = GraphBuilder.RegisterExternalTexture(
						CreateRenderTarget(RTResource->GetShaderResourceTexture(), TEXT("FogOfWar.VisionFieldExternal")));
					bPublishToExternal = true;
				}
			}

			if (bPublishToExternal)
			{
				LastFieldPublishFamily = Family;
				LastFieldPublishFrameNumber = FrameNumber;
			}
		}

		if (!VisionFieldTexture)
		{
			VisionFieldTexture = GraphBuilder.CreateTexture(
				FRDGTextureDesc::Create2D(
					FieldExtent, PF_R8G8, FClearValueBinding::Black,
					TexCreate_RenderTargetable | TexCreate_ShaderResource),
				TEXT("FogOfWar.VisionField"));
		}
	}

	auto* PackParameters = GraphBuilder.AllocParameters<FFogOfWarSceneVisionFieldPackPS::FParameters>();
	// 整张场都会被写满，因此不需要保留上一帧内容。
	PackParameters->RenderTargets[0] = FRenderTargetBinding(VisionFieldTexture, ERenderTargetLoadAction::ENoAction);
	PackParameters->VisionCoverageTexture = CoverageTexture;
	PackParameters->VisionExploredBits = ExploredBitsSRV;
	PackParameters->VisionExploredExtent = ExploredExtent;
	PackParameters->FieldTexelCount = FieldExtent;
	PackParameters->VisionCoverageScale = VisionCoverageScale;

	// 全屏顶点由 FPixelShaderUtils 自带的 VS 提供：打包趟只关心像素，不必自备一个空的顶点阶段。
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		ShaderMap,
		RDG_EVENT_NAME("FogOfWar.VisionFieldPack"),
		PackPixelShader,
		PackParameters,
		FIntRect(0, 0, FieldExtent.X, FieldExtent.Y));

	// ---- 趟 3：合成 ----

	const FIntPoint SceneColorExtent = SceneColor.Texture->Desc.Extent;

	auto* CompositePassParameters = GraphBuilder.AllocParameters<FFogOfWarSceneCompositePassParameters>();
	CompositePassParameters->RenderTargets[0] = Output.GetRenderTargetBinding();
	CompositePassParameters->SceneColorTexture = SceneColor.Texture;
	// 场景纹理 uniform buffer（含深度）：直接沿用引擎在后处理链上准备好的那一份。
	CompositePassParameters->SceneTexturesStruct = Inputs.SceneTextures.SceneTextures;
	CompositePassParameters->VisionFieldTexture = VisionFieldTexture;

	// 反投影矩阵只在像素阶段用（顶点阶段交给光栅器插值的必须是屏幕空间线性量），因此它属于
	// CompositePS 的参数结构；VS 那边只剩一个 UV 换算，这是"每个入口点各自的参数结构"的直接结果。
	const FMatrix44f ClipToWorld = FMatrix44f(View.ViewMatrices.GetClipToWorld());

	FFogOfWarSceneCompositeVS::FParameters CompositeVSParameters;
	CompositeVSParameters.SceneColorUVScaleBias = FVector4f(
		static_cast<float>(SceneColor.ViewRect.Width()) / static_cast<float>(SceneColorExtent.X),
		static_cast<float>(SceneColor.ViewRect.Height()) / static_cast<float>(SceneColorExtent.Y),
		static_cast<float>(SceneColor.ViewRect.Min.X) / static_cast<float>(SceneColorExtent.X),
		static_cast<float>(SceneColor.ViewRect.Min.Y) / static_cast<float>(SceneColorExtent.Y));

	FFogOfWarSceneCompositePS::FParameters CompositePSParameters;
	CompositePSParameters.InvViewProjection = ClipToWorld;
	CompositePSParameters.FogPlaneZ = RenderThreadSettings.PlaneZ;
	CompositePSParameters.FieldWorldMin = FieldWorldMin;
	CompositePSParameters.FieldWorldExtent = FieldWorldExtent;
	CompositePSParameters.SceneColorTexture = SceneColor.Texture;
	CompositePSParameters.SceneTexturesStruct = Inputs.SceneTextures.SceneTextures;
	CompositePSParameters.SceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	CompositePSParameters.VisionFieldTexture = VisionFieldTexture;
	CompositePSParameters.VisionFieldSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	CompositePSParameters.SceneFogNotVisibleRegionBrightness = FMath::Clamp(RenderThreadSettings.NotVisibleRegionBrightness, 0.0f, 1.0f);
	// 已探索区必须不比未探索区更暗，否则"记忆"反而把画面压得更黑 —— 夹一次比拼参数的人自觉更可靠。
	CompositePSParameters.SceneFogExploredRegionBrightness = FMath::Clamp(
		RenderThreadSettings.ExploredRegionBrightness,
		CompositePSParameters.SceneFogNotVisibleRegionBrightness,
		1.0f);
	CompositePSParameters.SceneFogOpacity = FMath::Clamp(RenderThreadSettings.Opacity, 0.0f, 1.0f);

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
