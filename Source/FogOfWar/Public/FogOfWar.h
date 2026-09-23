// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MassEntityTypes.h"
#include "Components/PostProcessComponent.h"
#include "MassFogOfWarFragments.h"
#include "Subsystems/MinimapDataSubsystem.h"
#include "FogOfWar.generated.h"

/// @file FogOfWar.h
/// @brief 定义了战争迷雾系统的核心Actor AFogOfWar。

class UTexture2D;

/**
 * @struct FFogVisionSource
 * @brief CPU 侧一条视野源：揭雾中心（世界 XY）+ 生效半径（cm）。
 * @details 与上传给后处理材质的是同一批数据：已按观察队伍过滤，且半径已叠加
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
};

/// 声明一个全局的日志分类，用于本模块的日志输出
DECLARE_LOG_CATEGORY_EXTERN(LogFogOfWar, Log, All)

/**
 * @class AFogOfWar
 * @brief 战争迷雾系统的核心管理器Actor。
 * @details 场景战争迷雾采用 GPU 圆形视野源后处理。CPU 从 MassBattle HashGrid 收集带
 * FMassVisionFragment 的 Agent 视野源（全图遍历、按观察队伍过滤、受 MaxSceneGpuVisionSources 上限约束），
 * 把 (WorldX, WorldY, SightRadius + SceneGpuVisionSourceRadiusPadding) 上传给后处理材质；
 * 同一批视野源也按队伍暴露给 CPU 侧消费者（CollectVisionSourcesByTeam，供探索层等逻辑累积历史）。
 */
UCLASS(BlueprintType, Blueprintable)
class FOGOFWAR_API AFogOfWar : public AActor
{
	GENERATED_BODY()

public:
	AFogOfWar();

public:
	/**
	 * @brief       为动态材质实例（MID）设置通用的着色器参数。
	 * @details     将网格尺寸、分辨率等通用信息传递给指定的MID。
	 * @param       MID                            数据类型: UMaterialInstanceDynamic*
	 * @details     需要设置参数的动态材质实例。
	 */
	UFUNCTION(BlueprintCallable)
	void SetCommonMIDParameters(UMaterialInstanceDynamic* MID);

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
	
	/// @brief 用于应用战争迷雾效果的后期处理组件。
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
	TObjectPtr<UPostProcessComponent> PostProcess;

	/// @brief 如果为true，系统将在BeginPlay时自动激活。
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bAutoActivate = true;

	/// @brief 世界网格范围（以世界坐标中心点 + 尺寸定义）。
	/// @details 当前无“边界盒”语义：这是坐标归一化参数，不做几何裁剪。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FogOfWar|Bounds", meta = (ClampMin = "1.0", UIMin = "1.0"))
	FVector2D WorldGridSize = FVector2D(409600.0f, 409600.0f);

	/// @brief 非可见区域的亮度。
	/// @details 在后期处理材质中，用于控制完全被迷雾覆盖区域的最终显示亮度。
	UPROPERTY(EditAnywhere, meta = (ClampMin = 0.0f, UIMin = 0.0f, ClampMax = 1.0f, UIMax = 1.0f))
	float NotVisibleRegionBrightness = 0.1f;

	/// @brief 唯一的场景战争迷雾后处理材质。材质读取圆形视野源并在 GPU 上逐像素揭雾。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Materials")
	TObjectPtr<UMaterialInterface> PostProcessingMaterial;

	/// @brief 为场景后处理材质提供逐像素 GPU 揭雾源。它独立于小地图战争迷雾。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU")
	bool bEnableSceneGpuVisionSources = true;

	/// @brief 传给场景后处理材质的最大视野源数量。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "1", UIMin = "1"))
	int32 MaxSceneGpuVisionSources = 4096;

	/// @brief 上传给 GPU 的每个视野源额外半径。用于抵消 hash/cell/材质采样边缘误差，避免漏视野。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float SceneGpuVisionSourceRadiusPadding = 300.0f;

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
	 * @details     在激活时调用，负责创建网格、扫描地形高度、创建所有纹理和材质实例。
	 */
	void Initialize();

	/**
	 * 更新场景后处理专用的 Mass 视野源纹理。
	 * @details 只收集与当前观察队伍（UMassBattleGlobalVarFunctionLibrary::GetTeam）同队的单位视野；
	 *          队伍不可用（INDEX_NONE）时退化为不按队伍过滤。
	 */
	void UpdateSceneGpuVisionSourceTexture();

	/**
	 * @brief       按队伍收集 CPU 侧视野源（一次遍历分桶）。
	 * @details     与 UpdateSceneGpuVisionSourceTexture() 走同一套规则（唯一实现见 .cpp 内的
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

	/// @brief PostProcessingMaterial的动态实例。
	UPROPERTY()
	TObjectPtr<UMaterialInstanceDynamic> PostProcessingMID;

	/// @brief 场景后处理材质读取的视野源数据纹理；每个 texel 为 (WorldX, WorldY, SightRadius, Reserved)。
	UPROPERTY(VisibleInstanceOnly, Category = "FogOfWar|Textures")
	TObjectPtr<UTexture2D> SceneGpuVisionSourceTexture = nullptr;

	/// @brief 避免每帧重复分配的场景视野源上传缓冲。
	TArray<FLinearColor> SceneGpuVisionSourceDataBuffer;

	/// @brief 当前已写入 SceneGpuVisionSourceTexture 的视野源数量。
	int32 SceneGpuVisionSourceCount = 0;

	int32 SceneGpuVisionPerfSampleCount = 0;
	double SceneGpuVisionPerfLastFlushTime = 0.0;
	float SceneGpuVisionPerfTotalMsAccum = 0.0f;
	float SceneGpuVisionPerfCollectMsAccum = 0.0f;
	float SceneGpuVisionPerfUploadMsAccum = 0.0f;
	int32 SceneGpuVisionPerfSourceCountAccum = 0;
	int32 SceneGpuVisionPerfVisitedCellsAccum = 0;
	int32 SceneGpuVisionPerfVisitedAgentsAccum = 0;

	void RecordSceneGpuVisionPerfStats(float TotalMs, float CollectMs, float UploadMs, int32 VisitedCells, int32 VisitedAgents);
	void FlushSceneGpuVisionPerfStats(double CurrentTime);
	void AppendSceneGpuVisionPerfCsvLine(const FString& CsvColumns) const;

	/// @brief 标记系统是否已激活。
	bool bActivated = false;
};
