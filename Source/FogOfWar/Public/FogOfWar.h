// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassEntityTypes.h"
#include "MassFogOfWarFragments.h"
#include "Subsystems/MinimapDataSubsystem.h"
#include "FogOfWar.generated.h"

/// @file FogOfWar.h
/// @brief 定义了战争迷雾系统的核心Actor AFogOfWar。

/** 场景雾的渲染实现（定义在 Private/SceneFog 下）：世界空间视野场散射 + R8G8 打包 + 合成。 */
class FFogOfWarSceneViewExtension;

/**
 * @struct FFogVisionSource
 * @brief CPU 侧一条视野源：揭雾中心（世界 XY）+ 生效半径（cm）。
 * @details 与交给场景雾渲染的是同一批数据：已按观察队伍过滤，且半径已叠加
 *          AFogOfWar::SceneGpuVisionSourceRadiusPadding（即实际被揭开的范围）。
 *          供同工程的 CPU 消费者（如地图的"已探索"层）复用同一条收集链，
 *          避免各自复刻"谁能看见"的规则而产生分叉。
 */
struct FOGOFWAR_API FFogVisionSource
{
	/// @brief 揭雾中心的世界 XY 坐标（cm）。
	FVector2D WorldLocation = FVector2D::ZeroVector;

	/// @brief 生效揭雾半径（cm，已含 SceneGpuVisionSourceRadiusPadding）。
	float RadiusCm = 0.0f;

	/// @brief 原始视距（cm，= FMassVisionFragment::SightRadius，**未**叠加 SceneGpuVisionSourceRadiusPadding）。
	/// @details 与 RadiusCm 是两个口径，用途不同，不要混用：
	///          - RadiusCm：渲染/揭雾口径，含投影余量，用于"被揭开的画面范围"（探索层累积用这个，
	///            否则探索层会比画面小一圈）；
	///          - 本字段：模型口径，"这个单位能看多远"。需要**当前可见性判定**的 CPU 消费者
	///            （如地图的当前可见层 → RL 观测的 bVisible）必须用本字段 —— 余量是给抖动/投影留的
	///            缓冲（默认 300cm），拿它做可见性判定等于凭空放大视野，让"看不见的敌人"被标成可见。
	float SightRadiusCm = 0.0f;
};

/// 声明一个全局的日志分类，用于本模块的日志输出
DECLARE_LOG_CATEGORY_EXTERN(LogFogOfWar, Log, All)

/**
 * @class AFogOfWar
 * @brief 战争迷雾系统的核心管理器Actor。
 * @details 场景战争迷雾的 GPU 路径：CPU 从 MassBattle HashGrid 收集带 FMassVisionFragment 的
 * Agent 视野源（全图遍历、按观察队伍过滤、视图剔除 + 圆盘包含剔除，受 MaxSceneGpuVisionSources
 * 上限约束），把 (WorldX, WorldY, SightRadius + SceneGpuVisionSourceRadiusPadding) 连同视野场几何
 * 交给 FFogOfWarSceneViewExtension，由它在 Tonemap 之后散射成世界空间视野场再合成（详见该类的说明）。
 * 同一批视野源也按队伍暴露给 CPU 侧消费者（CollectVisionSourcesByTeam，供探索层等逻辑累积历史）。
 *
 * @details “历史已探索”不由本类持有：本插件只维护当前帧可见性，历史由外部权威系统累积
 *          （本工程里是 UMassBattleMapSubsystem 的逐队探索层），经 FFogOfWarExploredLayerProvider
 *          把位图借给渲染侧当灰雾通道用。没有提供者时画面退化为“可见 / 不可见”二态。
 */
UCLASS(BlueprintType, Blueprintable)
class FOGOFWAR_API AFogOfWar : public AActor
{
	GENERATED_BODY()

public:
	AFogOfWar();

public:
	/**
	 * @brief       手动激活战争迷雾系统。
	 * @details     开始计算和渲染战争迷雾。如果bAutoActivate为true，则会在BeginPlay时自动调用。
	 */
	UFUNCTION(BlueprintCallable)
	void Activate();

	/**
	 * @brief       检查战争迷雾系统当前是否已激活。
	 * @return      bool
	 * @retval      true 如果已激活。
	 */
	UFUNCTION(BlueprintCallable, Category = "FogOfWar")
	bool IsActivated() const { return bActivated; }

public:
	//~ Begin UPROPERTY Configuration
	
	/// @brief 如果为true，系统将在BeginPlay时自动激活。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bAutoActivate = true;

	/// @brief 世界网格范围（以世界坐标中心点 + 尺寸定义）。
	/// @note 视野场需要一块覆盖地图范围的规则网格，而地图范围的首选来源是外部探索层
	///       （FFogOfWarExploredLayerProvider 给出的矩形，它同时决定已探索层的对齐）。只有拿不到
	///       提供者时才退回本属性 —— 那时场分辨率会按 VisionFieldTexelSizeCm 与每轴上限自适应，
	///       因此这里给一个偏大的缺省值是安全的，不会把场撑爆。
	/// @details 本属性同时是坐标归一化参数（供依赖世界范围的外部逻辑使用），本身不做几何裁剪。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FogOfWar|Bounds", meta = (ClampMin = "1.0", UIMin = "1.0"))
	FVector2D WorldGridSize = FVector2D(409600.0f, 409600.0f);

	/// @brief 从未探索区域（完全被迷雾覆盖）的亮度：0 = 全黑，1 = 不压暗。
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0.0f, UIMin = 0.0f, ClampMax = 1.0f, UIMax = 1.0f))
	float NotVisibleRegionBrightness = 0.1f;

	/// @brief 已探索、但当前不可见区域的亮度 —— 经典的“灰雾”。
	/// @details 这一档来自外部权威探索层（FFogOfWarExploredLayerProvider）；没有提供者时它不是一条
	///          无效配置，而是“永远不会被采样到”的旋钮，画面退化为“可见 / 不可见”二态。
	///          低于 NotVisibleRegionBrightness 时会被自动抬到该值：记忆不该比完全没去过还黑。
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0.0f, UIMin = 0.0f, ClampMax = 1.0f, UIMax = 1.0f))
	float ExploredRegionBrightness = 0.35f;

	/// @brief 战争迷雾遮蔽区域的不透明度：0 = 完全透明（看不出有雾），1 = 完全不透明（按上面的亮度压暗）。
	/// @details 与两个 Brightness 是正交的两件事：亮度决定“被遮住的地方压多暗”，本参数决定“这层遮蔽有多浓”。
	///          调低它会让未探索与已探索两档同时变淡，两者的相对明暗关系保持不变；当前可见的像素
	///          本来就不被遮蔽，因此不受本参数影响。
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0.0f, UIMin = 0.0f, ClampMax = 1.0f, UIMax = 1.0f))
	float SceneFogOpacity = 0.5f;

	/// @brief 场景雾的 GPU 揭雾源总开关。关闭时本帧交出空清单，渲染侧连 pass 都不注入（雾消失）。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU")
	bool bEnableSceneGpuVisionSources = true;

	/// @brief 每帧最多交给 GPU 的视野源数量，超出时按 HashGrid 遍历顺序截断。
	/// @note 新实现里这个上限只决定"每帧散射多少个圆盘"，不再像旧材质那样直接决定内层循环次数，
	///       因此它从"性能生死线"降级为"异常情况下的保险丝"。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "1", UIMin = "1"))
	int32 MaxSceneGpuVisionSources = 4096;

	/// @brief 交给 GPU 的每个视野源额外半径。用于抵消 hash/cell/投影边缘误差，避免漏视野。
	/// @details 这个余量同时吸收了"视野圆按世界 Z 平面投影、而像素所在表面可能高出一截"带来的
	///          屏幕偏移（见 SceneFogWorldPlaneZ），因此不建议调到 0。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float SceneGpuVisionSourceRadiusPadding = 300.0f;

	/// @brief 视野圆边缘的软化宽度（cm）。0 = 硬边。
	/// @details 覆盖率 = 1 - smoothstep(Radius - FogEdgeWidth, Radius, Distance)，圆内 1、圆外 0。
	///          每帧随参数快照交给渲染线程，因此在 PIE 里实时可调。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "0.0", UIMin = "0.0", Units = "cm"))
	float FogEdgeWidth = 0.0f;

	/// @brief 视野圆所在的世界 Z 平面（cm）。
	/// @details 揭雾圆盘投影在这个平面上。像素所在表面高出它 h 时会有约 h / tan(俯仰角) 的屏幕偏移，
	///          该偏移由 SceneGpuVisionSourceRadiusPadding 覆盖。地图整体抬高时把它设成地面高度即可
	///          消除偏移；默认 0 即地面基准面。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (Units = "cm"))
	float SceneFogWorldPlaneZ = 0.0f;

	/// @brief 视野场的纹素世界边长（cm）：场分辨率 ≈ 地图尺寸 / 这个值。
	/// @details 场是“当前覆盖率 + 历史已探索”的载体，它的分辨率与视口分辨率无关 —— 这是覆盖率改算在
	///          世界空间之后才成立的性质。地图范围异常大时分辨率会按上限等比放大（纹素边长随之变大），
	///          因此这里的值只是一个期望值，调小会让散射的纹素量按平方增长，调大会让雾边变块状。
	///          默认 64cm：一个 4000cm 的视野半径约 62 个纹素，软化边缘足够平滑。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "1.0", UIMin = "1.0", Units = "cm"))
	float VisionFieldTexelSizeCm = 64.0f;

	/// @brief 是否剔除完全落在当前视图之外的视野源。
	/// @details 合成阶段只会采样屏幕像素对应的世界点，而它们全部落在视锥内，所以完全落在视锥之外的圆
	///          不可能影响任何一个输出像素 —— 这是集合等价，不是近似。
	///          散射成本与源数线性相关（每条源一个 compute 线程组），因此这个开关的作用从
	///          "性能生死线"变成"减少无谓的线程组与源上传"；地图远大于屏幕可视范围时依然值得开着。
	///          判定所需的相机信息不可用时自动整体放弃剔除（宁可多留源，也绝不误剔）。
	/// @note 剔除只影响“当前这一帧画什么”，不影响历史已探索层 —— 后者由外部系统用不剔除的
	///       CollectVisionSourcesByTeam 累积，因此屏幕被移出视野的单位仍然会被记进探索层。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU")
	bool bCullVisionSourcesOutOfView = true;

	/// @brief 视图剔除的额外半径余量（cm）。
	/// @details 判据本身是几何严格的，但有两个必须让出余量的现实误差：① 相机缓存给的是上一帧
	///          的视图，而这里遍历的视野源位置是本帧的，两者差一帧 —— 相机高速平移时，上一帧
	///          视锥之外的源可能已进入本帧画面；② 视图宽高比在极端配置下可能与实际渲染视口
	///          略有出入。默认 500 cm 相对最小视野半径（1000 cm 起）只在视锥边缘多保留一条很薄
	///          的带，剔除收益基本不受影响；设为 0 会让这两种误差直接变成画面边缘缺一块揭雾。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float VisionSourceCullExtraMarginCm = 500.0f;

	UPROPERTY(EditAnywhere, Category = "FogOfWar|Performance")
	bool bEnableSceneGpuVisionPerformanceStats = true;

	UPROPERTY(EditAnywhere, Category = "FogOfWar|Performance")
	bool bLogSceneGpuVisionPerformanceToOutputLog = false;

	UPROPERTY(EditAnywhere, Category = "FogOfWar|Performance")
	bool bWriteSceneGpuVisionPerformanceCsv = true;

	UPROPERTY(EditAnywhere, Category = "FogOfWar|Performance", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float SceneGpuVisionPerformanceLogInterval = 2.0f;

	//~ End UPROPERTY Configuration

protected:
	virtual void BeginPlay() override;

#if WITH_EDITOR
#endif

	virtual void Tick(float DeltaSeconds) override;

public:
	//~ Begin Core Logic Functions
	
	/**
	 * @brief       初始化战争迷雾系统。
	 * @details     在激活时调用，按 WorldGridSize 算出以 Actor 为中心的世界网格范围，
	 *              供依赖“世界范围”做坐标归一化的外部逻辑使用；它同时是视野场几何在拿不到外部
	 *              地图范围（FFogOfWarExploredLayerProvider）时的兜底 —— 那份范围更准，因为它
	 *              必须与逐格已探索层同矩形。
	 *              渲染资源（视野场、覆盖率场、探索层缓冲）全部由渲染线程按帧创建，这里不涉及。
	 */
	void Initialize();

	/**
	 * @brief 收集本帧的场景雾视野源，并交给 FFogOfWarSceneViewExtension。
	 * @details 只收集与当前观察队伍同队的单位视野。观察队伍由外部注册的提供者给出
	 *          （FFogOfWarViewingTeamProvider，本工程里注册的是
	 *          UMassBattleGlobalVarFunctionLibrary::GetTeam）；提供者未注册或返回
	 *          INDEX_NONE 时退化为不按队伍过滤。
	 *          收集不到（开关关闭 / 子系统缺失 / 世界正在销毁）时交出空清单，让渲染侧连
	 *          pass 都不注入，雾直接消失而不是停在上一帧的旧圆盘上。
	 */
	void UpdateSceneGpuVisionSources();

	/**
	 * @brief 收集所有本地玩家视图的剔除平面（世界空间；FPlane::PlaneDot 小于 0 视为视锥内侧）。
	 * @details 每个 View 贡献 4 个侧平面：透视按水平 FOV 与宽高比构造楔形，正交按 OrthoWidth 与
	 *          宽高比构造柱体。多 View（分屏）时平面取并集 —— 源只要还落在任一 View 内就不剔除。
	 *          透视/正交都不额外加近远平面：少了那两个平面只会漏剔（保守），不会误剔。
	 *          任一 View 拿不到可靠的宽高比时，直接整体放弃剔除，因为"用错误的宽高比算出的楔形"
	 *          比实际视锥更窄，会误剔掉屏幕边缘仍然可见的源。
	 * @param OutPlanes 输出平面列表；为空表示本次不做剔除。
	 * @return 是否得到了一组可用的剔除平面。
	 */
	bool BuildVisionSourceCullPlanes(TArray<FPlane>& OutPlanes) const;

	/** 视野圆（中心 + 半径）是否可能落在任一视图之内。平面为空时恒为 true（不剔除）。 */
	static bool IsVisionSourcePossiblyVisible(const TArray<FPlane>& InPlanes, const FVector& InCenter, float InRadiusCm);

	/**
	 * @brief       按队伍收集 CPU 侧视野源（一次遍历分桶）。
	 * @details     与 UpdateSceneGpuVisionSources() 走同一套规则（唯一实现见 .cpp 内的
	 *              TryGetVisionSourceRadius）：FMassVisionFragment::SightRadius 必须为正，
	 *              生效半径 = SightRadius + SceneGpuVisionSourceRadiusPadding，并按 FTeam::index 归队。
	 *              与 GPU 侧只有两点不同：① 不按"当前观察队伍"过滤，而是每个队伍各一份；
	 *              ② 不受 bEnableSceneGpuVisionSources 开关影响（那是渲染开关，而探索累积属于
	 *              逻辑/观测需求）。每队的收集上限同为 MaxSceneGpuVisionSources。
	 *              本插件只维护"当前帧可见性"，不保存历史探索状态 —— 需要"累积已探索"的
	 *              消费者（如地图探索层）应自行累积本接口给出的视野源。
	 * @param       OutSourcesByTeam        数据类型: TArray<TArray<FFogVisionSource>>&
	 * @details     输出：索引 = 队伍下标，长度 = InTeamCount（整体覆盖，不做增量追加）。
	 * @param       InTeamCount             需要分桶的队伍数；<=0 时输出为空。
	 * @param       OutHighestTeamIndexSeen 可选输出：本次遍历见到过的最大队伍下标（含未落桶的越界队伍），
	 *                                      供调用方自检"是否有队伍的视野超出了已配置队伍数"。
	 * @return      收集到的视野源总数（各桶之和；不含越界队伍与无队伍碎片的 Agent）。
	 */
	int32 CollectVisionSourcesByTeam(TArray<TArray<FFogVisionSource>>& OutSourcesByTeam, int32 InTeamCount, int32* OutHighestTeamIndexSeen = nullptr) const;
	//~ End Core Logic Functions

public:
	//~ Begin Internal State Properties

	/// @brief 网格在世界空间中的尺寸（宽和高）。
	UPROPERTY(VisibleInstanceOnly)
	FVector2D GridSize = FVector2D::Zero();

	/// @brief 网格左下角在世界空间中的2D坐标。作为所有坐标转换的基准。
	UPROPERTY(VisibleInstanceOnly)
	FVector2D GridBottomLeftWorldLocation = FVector2D::Zero();

	/// @brief 场景雾的渲染实现。Activate 时注册进引擎，成员释放即反注册。
	TSharedPtr<FFogOfWarSceneViewExtension, ESPMode::ThreadSafe> SceneFogViewExtension;

	/// @brief 本帧的视野源清单，元素为 (WorldX, WorldY, Radius, Reserved)。
	/// @details 每帧 Reset（保留容量）后 Add，Num 即本帧源数；交给渲染线程时整个数组被交换走，
	///          下一帧换回上一帧的缓冲继续复用，因此稳态下不产生分配。
	TArray<FVector4f> SceneGpuVisionSources;

	/// @brief 最近一次采用的已探索层版本号（由提供者给出）。
	/// @details 只有它变化时才重新取位图并交给渲染线程：探索层每秒只变几次，而这里是每帧一次。
	///          INDEX_NONE 表示“当前没有可用的已探索层”（无提供者 / 网格未就绪 / 观察队伍不可用），
	///          此时渲染侧收到的是一份空层，画面退化为“可见 / 不可见”二态。
	int32 SceneFogExploredLayerSourceVersion = INDEX_NONE;

	/**
	 * @brief 把当前视野源清单、参数快照与视野场几何交给渲染线程，并顺带刷新已探索层（未注册扩展时什么都不做）。
	 * @details 参数与源同帧一起过去，渲染线程只认这一份快照：既保证同帧一致，也让这些旋钮
	 *          （FogEdgeWidth / 两个亮度 / 投影平面 / 场纹素边长）在 PIE 里改动能立刻生效。
	 *
	 * @details 场几何优先取外部探索层给的地图范围（它与逐格探索层必须同矩形，否则灰雾会整体错位），
	 *          拿不到时才退回本 Actor 的 GridBottomLeftWorldLocation + GridSize。已探索层同样在这里
	 *          刷新：它每秒只变几次，靠提供者给出的版本号判断要不要重新取。
	 */
	void UploadSceneGpuVisionSources();

	int32 SceneGpuVisionPerfSampleCount = 0;
	double SceneGpuVisionPerfLastFlushTime = 0.0;
	float SceneGpuVisionPerfTotalMsAccum = 0.0f;
	float SceneGpuVisionPerfCollectMsAccum = 0.0f;
	float SceneGpuVisionPerfUploadMsAccum = 0.0f;
	int32 SceneGpuVisionPerfSourceCountAccum = 0;
	int32 SceneGpuVisionPerfVisitedCellsAccum = 0;
	int32 SceneGpuVisionPerfVisitedAgentsAccum = 0;

	/// @brief 统计周期内被视图剔除的视野源数累计。与 AvgSourceCount 对照即可读出剔除收益
	///        （散射的实例数正比于实际交给 GPU 的源数，而不是遍历到的源数）。
	int32 SceneGpuVisionPerfCulledSourcesAccum = 0;

	/// @brief 统计周期内被圆盘包含剔除的视野源数累计。判据是"圆盘完全落在另一个圆盘内"，
	///        属于集合等价删除（删掉它对画面严格无影响），与视图剔除同一性质；
	///        搜索范围限于同一个 HashGrid 格内，因此是"可靠的但未必穷尽"的剔除。
	int32 SceneGpuVisionPerfContainedSourcesAccum = 0;

	/// @param SourceCount 本帧交给渲染线程的源数。必须由调用方在交出清单之前算好 —— 清单是交换走的，
	///                    交出之后 SceneGpuVisionSources 里已经是上一帧的缓冲了。
	void RecordSceneGpuVisionPerfStats(float TotalMs, float CollectMs, float UploadMs, int32 VisitedCells, int32 VisitedAgents, int32 CulledSources, int32 ContainedSources, int32 SourceCount);
	void FlushSceneGpuVisionPerfStats(double CurrentTime);
	void AppendSceneGpuVisionPerfCsvLine(const FString& CsvColumns) const;

	/// @brief 标记系统是否已激活。
	bool bActivated = false;
};
