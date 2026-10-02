// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "SceneViewExtension.h"

class FRDGBuilder;
struct FScreenPassTexture;
struct FPostProcessMaterialInputs;

/**
 * @struct FFogOfWarSceneFogSettings
 * @brief 一帧的场景雾参数快照。
 * @details 与视野源一起从游戏线程交到渲染线程：渲染线程只认这一份快照，不会再去读 Actor 属性，
 *          因此不会出现“参数在游戏中改了、这一帧渲染用的却是别的值”这类跨线程漂移。
 */
struct FFogOfWarSceneFogSettings
{
	/// @brief 视野圆边缘软化宽度（cm）；0 = 硬边。
	float EdgeWidthCm = 0.0f;

	/// @brief 从未探索区域的亮度（0 = 全黑，1 = 不压暗）。
	float NotVisibleRegionBrightness = 0.1f;

	/// @brief 已探索、但当前不可见区域的亮度 —— 经典的“灰雾”。必须 >= NotVisibleRegionBrightness 才有意义。
	float ExploredRegionBrightness = 0.35f;

	/// @brief 雾层自身的不透明度：0 = 完全透明（看不见雾），1 = 完全不透明（按上面的亮度压暗）。
	/// @details 与两个 Brightness 正交：那两个决定“被遮住的地方压多暗”，本参数决定“这层遮蔽有多浓”。
	float Opacity = 0.5f;

	/// @brief 视野圆（以及整个视野场）所在的世界 Z 平面（cm）。见 AFogOfWar::SceneFogWorldPlaneZ。
	float PlaneZ = 0.0f;
};

/**
 * @struct FFogOfWarSceneVisionField
 * @brief 视野场的世界几何：一块覆盖地图范围、与相机无关的规则网格。
 *
 * @details 覆盖率算在世界空间而不是屏幕空间，带来三件事：
 *          ① 每条视野源的散射范围（AABB）是纯算术，不含任何相机投影，因此不会有“源落在相机后方 /
 *             掠射角导致屏幕 AABB 退化”这类必须在 CPU 端特判的边界情形；
 *          ② 与相机解耦 —— 多视口共享同一份场，且相机平移不会改变自己的 AABB；
 *          ③ 与“历史已探索”所在的逐格网格同世界矩形，G 通道的采样 UV 直接沿用场的 UV。
 */
struct FFogOfWarSceneVisionField
{
	/// @brief 场在世界空间中的最小角（XY，cm）。
	FVector2D WorldMin = FVector2D::ZeroVector;

	/// @brief 场在世界空间中的尺寸（XY，cm）。
	FVector2D WorldSize = FVector2D::ZeroVector;

	/// @brief 场分辨率（纹素数）。
	FIntPoint TexelCount = FIntPoint::ZeroValue;

	/// @brief 单个场纹素的世界边长（cm）= WorldSize / TexelCount（两轴同长）。
	float TexelSizeCm = 0.0f;

	/** 几何是否已就绪（着色器里的除法与 AABB 都要求它成立）。 */
	FORCEINLINE bool IsValid() const
	{
		return TexelCount.X > 0 && TexelCount.Y > 0 && TexelSizeCm > 0.0f
			&& WorldSize.X > 0.0f && WorldSize.Y > 0.0f;
	}
};

/**
 * @struct FFogOfWarSceneExploredLayer
 * @brief 历史已探索层的渲染线程副本：与原位图同布局的 LSB-first 位流。
 *
 * @details 数据来源是外部的权威探索层（见 FFogOfWarExploredLayerProvider），本插件不持有历史 ——
 *          这里只是一份“最近一次取到的快照”。版本号用于跳过每帧的重复拷贝：
 *          探索层每秒只变几次，而渲染每帧都要问一次。
 *
 * @details 为什么保持位打包而不展开成逐格字节：200×200 的位图是 5KB，展开成字节是 40KB，
 *          按 uint 展开是 160KB —— 而这几 KB/帧是每帧都要上传到 GPU 的，展开没有任何好处。
 *          位序与 FRlSpatialMap::ExploredBitmap 一致（LSB-first），因此从位图拷进来只需一次 memcpy。
 */
struct FFogOfWarSceneExploredLayer
{
	/// @brief LSB-first 位流（word i 的 bit b = 全局 bit i*32+b），长度 = ceil(Width*Height/32)。
	TArray<uint32> PackedBits;

	/// @brief 位图分辨率（格数）。
	FIntPoint Extent = FIntPoint::ZeroValue;

	/// @brief 内容版本号；INDEX_NONE 表示“从来没有过可用数据”。
	int32 Version = INDEX_NONE;

	/** 数据是否可用。不可用时渲染侧交出空 buffer，G 通道恒为 0（退化为二态雾）。 */
	FORCEINLINE bool IsValid() const
	{
		return Extent.X > 0 && Extent.Y > 0
			&& PackedBits.Num() > 0
			&& static_cast<int64>(PackedBits.Num()) * 32 >= static_cast<int64>(Extent.X) * Extent.Y;
	}
};

/**
 * @class FFogOfWarSceneViewExtension
 * @brief 场景战争迷雾的渲染实现：把视野源散射成世界空间视野场，再在后处理链的 Tonemap 之后合成。
 *
 * @details 三趟 pass（详见 FogOfWarScene.usf 顶部注释）：
 *          VisionFieldSplatCS    —— 每条视野源只在自己的场 AABB 内派发线程，用 InterlockedMax 写
 *                                   定标覆盖率。重叠圆盘不再各跑一遍像素着色，overdraw 被彻底消掉。
 *          VisionFieldPackVS/PS  —— 把整数覆盖率与历史已探索打包成一张 PF_R8G8
 *                                   （R = 当前视野覆盖率，G = 已探索）。
 *          CompositeVS/PS       —— 屏幕像素反投影到世界平面 → 采样 R8G8 → 可见 / 已探索但不可见 / 未探索。
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
	 * @brief 游戏线程：交出本帧的视野源列表、雾参数快照与视野场几何。
	 * @param InOutSources 视野源列表（每条一个 float4：世界 XY + 生效半径）。内容会被交换走，
	 *                     换回来的是上一帧用过的缓冲，交给调用方 Reset 后复用，两侧都不产生分配。
	 * @param InSettings   本帧雾参数快照。
	 * @param InField      本帧视野场几何（世界矩形 + 分辨率）。
	 * @param bInSceneFogActive 本帧是否应该用雾遮蔽画面 —— **与"有没有视野源"是两件事**：
	 *                     ① 雾系统被关掉 / 子系统不可用 → false：渲染侧完全不注入 pass，画面不受遮蔽
	 *                        （这是"我不想画雾"）；
	 *                     ② 雾系统正常、但本帧一条视野源都没有（例如相机移出了所有单位的视野范围，
	 *                        或视野源被视图剔除干净）→ true：仍然注入 pass，覆盖率场全 0，
	 *                        合成结果是"整屏从未探索" —— 即**全遮蔽**。这才是"什么都看不见"的正确表现，
	 *                        而不是让迷雾凭空消失。
	 * @note 这里只做一次加锁交换，不碰任何 RHI 资源：GPU 侧的缓冲/纹理全部在渲染线程按帧创建，
	 *       游戏线程不参与图形资源管理。
	 */
	void UploadFrameData_GameThread(
		TArray<FVector4f>& InOutSources,
		TArray<FVector4f>& InOutSourceDirs,
		const FFogOfWarSceneFogSettings& InSettings,
		const FFogOfWarSceneVisionField& InField,
		bool bInSceneFogActive);

	/**
	 * @brief 游戏线程：交出历史已探索层（只在内容真的变化时调用）。
	 * @param InExploredLayer 已展开成逐格 0/255 的快照；版本号与上一次相同则渲染侧会跳过拷贝。
	 * @note 与 UploadFrameData_GameThread 分开，是因为它的数据量（几十 KB）比视野源大两个数量级，
	 *       不值得每帧搬一遍。
	 */
	void UploadExploredLayer_GameThread(const FFogOfWarSceneExploredLayer& InExploredLayer);

private:
	/**
	 * @brief 渲染线程：Tonemap 之后插入“散射场 + 打包 + 合成”三趟 pass。
	 * @details 若本帧不应遮蔽（雾系统关闭）或场几何不可用，则直接返回输入场景色，连一次全屏 pass
	 *          都不产生；而“雾该画、只是本帧没有视野源”**不**在这里早退 —— 那正是需要跑一趟、
	 *          把整屏按“从未探索”遮蔽掉的情况。
	 */
	FScreenPassTexture PostProcess_RenderThread(
		FRDGBuilder& GraphBuilder,
		const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/// @brief 游戏线程写入 / 渲染线程读取的交换区。
	FCriticalSection PendingLock;
	TArray<FVector4f> PendingSources;

	/// @brief 与 PendingSources 同索引的扇形参数 (dirX, dirY, cosHalfAngle, 0)；cosHalfAngle >= 1 = 全向。
	/// @details 与 PendingSources 一起交换，保证渲染线程看到的两条数组永远同帧同索引。
	TArray<FVector4f> PendingSourceDirs;

	FFogOfWarSceneFogSettings PendingSettings;
	FFogOfWarSceneVisionField PendingField;
	FFogOfWarSceneExploredLayer PendingExploredLayer;

	/** 本帧是否应该用雾遮蔽画面（与"有没有视野源"是两件事，见 UploadFrameData_GameThread）。
	 *  false = 雾系统关闭 / 子系统不可用 → 渲染侧完全不注入 pass，画面不受遮蔽。 */
	bool PendingSceneFogActive = true;

	/**
	 * @brief 渲染线程私有的本帧快照。
	 * @details SubscribeToPostProcessingPass 与随后的回调都在渲染线程执行，且在同一帧内先后发生，
	 *          中间不会插入下一帧的 Subscribe（多视口时同一帧会 Subscribe 多次，因此只能拷贝、
	 *          不能把 PendingSources 换走），所以这里不需要额外加锁。
	 */
	TArray<FVector4f> RenderThreadSources;
	FFogOfWarSceneFogSettings RenderThreadSettings;
	FFogOfWarSceneVisionField RenderThreadField;

	/// @brief 渲染线程手里的本帧扇形参数快照，与 RenderThreadSources 同索引、同帧。
	TArray<FVector4f> RenderThreadSourceDirs;

	/** 渲染线程手里的"本帧是否应遮蔽"快照（含义见 UploadFrameData_GameThread）。 */
	bool RenderThreadSceneFogActive = true;

	/// @brief 渲染线程手里的已探索层快照；只在 Pending 的版本号变化时才重新拷贝（见 UploadExploredLayer_GameThread）。
	FFogOfWarSceneExploredLayer RenderThreadExploredLayer;
};
