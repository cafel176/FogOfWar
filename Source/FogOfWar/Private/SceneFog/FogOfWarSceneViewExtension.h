// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SceneViewExtension.h"

class FRDGBuilder;
class FScreenPassTexture;
struct FPostProcessMaterialInputs;

/**
 * @struct FFogOfWarSceneFogSettings
 * @brief 一帧的场景雾参数快照。
 * @details 与视野源一起从游戏线程交到渲染线程：渲染线程只认这一份快照，不会再去读 Actor 属性，
 *          因此不会出现"参数在游戏中改了、这一帧渲染用的却是别的值"这类跨线程漂移。
 */
struct FFogOfWarSceneFogSettings
{
	/// @brief 视野圆边缘软化宽度（cm）；0 = 硬边。
	float EdgeWidthCm = 0.0f;

	/// @brief 完全被雾覆盖区域的亮度（0 = 全黑，1 = 不压暗）。
	float NotVisibleRegionBrightness = 0.1f;

	/// @brief 视野圆所在的世界 Z 平面（cm）。见 AFogOfWar::SceneFogWorldPlaneZ。
	float PlaneZ = 0.0f;

	/// @brief 覆盖率遮罩的分辨率分母：1 = 与视口同分辨率，2 = 半分辨率。
	int32 MaskResolutionDivisor = 1;
};

/**
 * @class FFogOfWarSceneViewExtension
 * @brief 场景战争迷雾的渲染实现：把视野圆盘散射成屏幕空间覆盖率遮罩，再在后处理链的 Tonemap 之后合成。
 *
 * @details 为什么从"后处理材质"换成 SceneViewExtension + RDG：
 *          ① 材质图没有 scatter 能力（一个像素不能写多个像素，也不能读结构化缓冲做原子追加），
 *             于是"每个屏幕像素遍历全部视野源"是材质路线唯一能表达的形式，成本是
 *             O(屏幕像素数 × 源数) —— 1080p × 500 源就是约 1e9 次内层迭代；
 *          ② 代码路线可以按圆盘散射（每条视野源一个实例化多边形），成本是
 *             O(Σ 圆盘覆盖的屏幕面积 + 屏幕像素数)，与源数线性、与屏幕像素没有乘积项；
 *          ③ 参数从此只有一个真值源（C++ 属性 + usf），不再有"材质资产里的默认值"和
 *             "C++ 每帧推送的值"两份可能分叉的契约（FOW_FogEdgeWidth 先前就是这样被漏掉的）。
 *
 * @details 生命周期：由 AFogOfWar 持有 TSharedPtr，Activate 时注册，最后一个引用释放即反注册。
 *          注册是在游戏线程做的，但 SceneViewExtension 的注册表对每一帧都会留引用到渲染结束，
 *          因此渲染线程在整帧内看到的对象始终有效。
 */
class FFogOfWarSceneViewExtension final : public FWorldSceneViewExtension
{
public:
	FFogOfWarSceneViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	//~ Begin ISceneViewExtension
	virtual void SubscribeToPostProcessingPass(
		EPostProcessingPass Pass,
		const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks,
		bool bIsPassEnabled) override;
	//~ End ISceneViewExtension

	/**
	 * @brief 游戏线程：交出本帧的视野源列表与雾参数。
	 * @param InOutSources 视野源列表（每条一个 float4：世界 XY + 生效半径）。内容会被交换走，
	 *                     换回来的是上一帧用过的缓冲，交给调用方 Reset 后复用，两侧都不产生分配。
	 * @param InSettings   本帧雾参数快照。
	 * @note 这里只做一次加锁交换，不碰任何 RHI 资源：GPU 侧的缓冲/纹理全部在渲染线程按帧创建，
	 *       游戏线程不参与图形资源管理（旧实现每帧在游戏线程重建并上传一张纹理）。
	 */
	void UploadFrameData_GameThread(TArray<FVector4f>& InOutSources, const FFogOfWarSceneFogSettings& InSettings);

private:
	/**
	 * @brief 渲染线程：Tonemap 之后插入"散射遮罩 + 合成"两趟 pass。
	 * @details 若本帧没有视野源，则直接返回输入场景色，连一次全屏 pass 都不产生。
	 */
	FScreenPassTexture PostProcess_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/// @brief 游戏线程写入 / 渲染线程读取的交换区。
	FCriticalSection PendingLock;
	TArray<FVector4f> PendingSources;
	FFogOfWarSceneFogSettings PendingSettings;

	/**
	 * @brief 渲染线程私有的本帧快照。
	 * @details SubscribeToPostProcessingPass 与随后的回调都在渲染线程执行，且在同一帧内先后发生，
	 *          中间不会插入下一帧的 Subscribe（多视口时同一帧会 Subscribe 多次，因此只能拷贝、
	 *          不能把 PendingSources 换走），所以这里不需要额外加锁。
	 */
	TArray<FVector4f> RenderThreadSources;
	FFogOfWarSceneFogSettings RenderThreadSettings;
};
