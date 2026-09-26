// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWar.h"

#include "FogOfWarMassBinding.h"
#include "Camera/CameraTypes.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "MassEntitySubsystem.h"
#include "SceneFog/FogOfWarSceneViewExtension.h"
#include "SceneViewExtension.h"
#include "Subsystems/MassBattleHashGridSubsystem.h"
#include "Subsystems/MinimapDataSubsystem.h"
#include "FogOfWarViewingTeamProvider.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogFogOfWar);

DECLARE_STATS_GROUP(TEXT("FogOfWar"), STATGROUP_FogOfWar, STATCAT_Advanced);

namespace Names
{
	const TCHAR* ScenePerformanceCsvRelativePath = TEXT("Logs/FogOfWar_ScenePerf.csv");

	// 旧后处理材质路线的全部材质参数名（FOW_*）与那张视野源纹理，已随材质一起删除：
	// 场景雾现在由 FFogOfWarSceneViewExtension 的两趟 pass 实现（覆盖率遮罩 + 合成），
	// 参数只有 C++ 属性这一个真值源，不再存在"材质资产里的默认值"与"C++ 每帧推送的值"
	// 两份可能分叉的契约（FOW_FogEdgeWidth 先前就是这样被漏掉的）。
}

namespace
{
	/**
	 * 圆盘包含判定：InA 的圆盘是否完全落在 InB 的圆盘之内（等价于 dist(A,B) + rA <= rB）。
	 *
	 * 为什么删掉被包含的源是严格等价的：散射遮罩取的是所有源的覆盖率最大值（max 混合），A 若能揭开某个像素，
	 * 则覆盖该像素的 B 一定能揭开它、且覆盖率不低于 A。所以这是集合等价，不是近似 —— 与视图剔除
	 * 同一性质（精确优化），而不是"看起来差不多"的启发式裁剪。
	 *
	 * 入参约定：x/y 为世界 XY，z 为生效揭雾半径（已含 SceneGpuVisionSourceRadiusPadding），w 未用。
	 * 注意：等半径的两个源只有在完全同心（含同一位置重复出现的源）时才会互相判定为包含。
	 */
	bool IsDiscContainedIn(const FVector4f& InA, const FVector4f& InB)
	{
		const float RadiusDelta = InB.Z - InA.Z;
		if (RadiusDelta < 0.0f)
		{
			return false;
		}

		// 半径差就是圆心距的上界：先按轴各比一次，绝大多数源对在这里就被滤掉，不必开方，也不必算距离平方。
		const float DeltaX = InA.X - InB.X;
		if (FMath::Abs(DeltaX) > RadiusDelta)
		{
			return false;
		}

		const float DeltaY = InA.Y - InB.Y;
		if (FMath::Abs(DeltaY) > RadiusDelta)
		{
			return false;
		}

		return (DeltaX * DeltaX + DeltaY * DeltaY) <= (RadiusDelta * RadiusDelta);
	}

	/**
	 * 视野源判定（全插件唯一实现）：该 Agent 是否为指定队伍的视野源，并给出生效揭雾半径。
	 * GPU 揭雾收集（UpdateSceneGpuVisionSources）与 CPU 侧逐队收集（CollectVisionSourcesByTeam）
	 * 都调用这里，保证"谁能看见"的规则只有一份，不会在两条链路之间漂移。
	 *
	 * @param InEntityManager   实体管理器。
	 * @param InAgentData       HashGrid 中的 Agent 数据（实体句柄 + 相对格心偏移）。
	 * @param InTeamIndex       队伍下标；INDEX_NONE 表示不按队伍过滤（全场并集）。
	 * @param InRadiusPaddingCm 生效半径余量（AFogOfWar::SceneGpuVisionSourceRadiusPadding）。
	 * @param OutRadiusCm       输出：生效揭雾半径 = SightRadius + 余量。
	 * @param OutTeamIndex      可选输出：该 Agent 的队伍下标（无队伍碎片时为 INDEX_NONE）。
	 * @return 是否为有效视野源。
	 */
	bool TryGetVisionSourceRadius(
		const FMassEntityManager& InEntityManager,
		const FAgentGridData& InAgentData,
		int32 InTeamIndex,
		float InRadiusPaddingCm,
		float& OutRadiusCm,
		int32* OutTeamIndex = nullptr)
	{
		if (!InEntityManager.IsEntityValid(InAgentData.EntityHandle))
		{
			return false;
		}

		// ① 必须有视野碎片，且视距为正（SightRadius <= 0 视为不揭雾）。
		const FMassVisionFragment* VisionFragment = InEntityManager.GetFragmentDataPtr<FMassVisionFragment>(InAgentData.EntityHandle);
		if (!VisionFragment || VisionFragment->SightRadius <= 0.0f)
		{
			return false;
		}

		// ② 队伍过滤：FTeam::index 与“观察队伍提供者”给出的下标同口径
		//    （FFogOfWarViewingTeamProvider，本工程注册的是 UMassBattleGlobalVarFunctionLibrary::GetTeam）。
		//    需要过滤或需要回传队伍下标时才读队伍碎片，避免给 GPU 那条热路径增加无谓开销。
		if (InTeamIndex != INDEX_NONE || OutTeamIndex != nullptr)
		{
			const FOW_TEAM_FRAGMENT* TeamFragment = InEntityManager.GetFragmentDataPtr<FOW_TEAM_FRAGMENT>(InAgentData.EntityHandle);
			const int32 AgentTeamIndex = TeamFragment ? FOW_GET_TEAM_INDEX(*TeamFragment) : INDEX_NONE;

			if (OutTeamIndex)
			{
				*OutTeamIndex = AgentTeamIndex;
			}
			if (InTeamIndex != INDEX_NONE && AgentTeamIndex != InTeamIndex)
			{
				return false;
			}
		}

		// ③ 生效半径 = 单体视距 + 场景揭雾余量（与交给 GPU 的口径一致，避免探索层比画面小一圈）。
		OutRadiusCm = VisionFragment->SightRadius + InRadiusPaddingCm;
		return true;
	}
}

AFogOfWar::AFogOfWar()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
}

void AFogOfWar::Activate()
{
	if (!ensure(!bActivated))
	{
		return;
	}
	bActivated = true;

	Initialize();

	UMinimapDataSubsystem* MinimapSubsystem = UMinimapDataSubsystem::Get();
	if (MinimapSubsystem)
	{
		MinimapSubsystem->SetVisionGridActive(false);
	}

	// 场景雾的 GPU 实现是一个"世界作用域"的 SceneViewExtension：注册之后，该世界所有视图的
	// Tonemap 之后都会插入 FFogOfWarSceneViewExtension 的两趟 pass（散射遮罩 + 合成）。
	// 反注册靠成员释放完成（引擎会把当帧引用留到渲染结束），不需要额外的 Deactivate。
	if (UWorld* World = GetWorld())
	{
		SceneFogViewExtension = FSceneViewExtensions::NewExtension<FFogOfWarSceneViewExtension>(World);
	}
	else
	{
		UE_LOG(LogFogOfWar, Warning, TEXT("Activate: 没有有效的 World，场景雾不会渲染（只有 CPU 侧视野源收集仍然可用）。"));
	}

	PrimaryActorTick.SetTickFunctionEnable(true);
}

void AFogOfWar::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoActivate)
	{
		Activate();
	}
}

void AFogOfWar::Tick(float DeltaSeconds)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("Tick"), STAT_FogOfWarTick, STATGROUP_FogOfWar);

	Super::Tick(DeltaSeconds);

	UpdateSceneGpuVisionSources();
}

void AFogOfWar::Initialize()
{
	GridSize = FVector2D::ZeroVector;
	GridBottomLeftWorldLocation = FVector2D::ZeroVector;
	const FVector Origin = GetActorLocation();
	GridSize = FVector2D(FMath::Max(1.0f, WorldGridSize.X), FMath::Max(1.0f, WorldGridSize.Y));
	GridBottomLeftWorldLocation = FVector2D(
		Origin.X - GridSize.X * 0.5f,
		Origin.Y - GridSize.Y * 0.5f);
	UE_LOG(LogFogOfWar, Log, TEXT("Using actor-centered grid for world bounds. Origin=%s Size=%s"),
		*GridBottomLeftWorldLocation.ToString(), *GridSize.ToString());

	// Keep AFogOfWar scene fog decoupled from minimap sizing.
	// Minimap now computes its own scale (or uses HashGrid bounds), not FogOfWar's world bounds.
}

bool AFogOfWar::BuildVisionSourceCullPlanes(TArray<FPlane>& OutPlanes) const
{
	OutPlanes.Reset();

	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		const APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr;
		if (!CameraManager)
		{
			continue;
		}

		// 相机缓存里是上一帧实际用于渲染的视图参数（位置、朝向、FOV / 正交宽度、宽高比）。
		const FMinimalViewInfo& POV = CameraManager->GetCameraCacheView();

		int32 ViewportSizeX = 0;
		int32 ViewportSizeY = 0;
		PlayerController->GetViewportSize(ViewportSizeX, ViewportSizeY);

		// 揭雾最终落在视口的实际像素上，因此宽高比优先取视口尺寸，视图自带值只作兜底。
		const float Aspect = (ViewportSizeX > 0 && ViewportSizeY > 0)
			? static_cast<float>(ViewportSizeX) / static_cast<float>(ViewportSizeY)
			: POV.AspectRatio;

		// 相机缓存尚未填过、或该投影模式的关键尺寸为 0 时，整体放弃剔除：用 0 当尺寸算出的视锥
		// 会退化，把当前可见的源全部剔掉。宁可这一帧不剔除，也绝不误剔。
		const bool bPOVValid = (Aspect > 0.0f)
			&& (POV.ProjectionMode == ECameraProjectionMode::Orthographic
				? POV.OrthoWidth > 0.0f
				: POV.FOV > 0.0f);
		if (!bPOVValid)
		{
			OutPlanes.Reset();
			return false;
		}

		const FRotationMatrix CameraBasis(POV.Rotation);
		const FVector CameraForward = CameraBasis.GetScaledAxis(EAxis::X);
		const FVector CameraRight = CameraBasis.GetScaledAxis(EAxis::Y);
		const FVector CameraUp = CameraBasis.GetScaledAxis(EAxis::Z);
		const FVector CameraOrigin = POV.Location;

		if (POV.ProjectionMode == ECameraProjectionMode::Orthographic)
		{
			// 正交视锥是一个无限延伸的柱体：屏幕覆盖区域与相机距离无关，四个侧平面就够。
			// RTS 俯视多用正交相机，这条分支必须与透视一样成立。
			const float HalfWidth = POV.OrthoWidth * 0.5f;
			const float HalfHeight = HalfWidth / Aspect;
			const float RightOffset = FVector::DotProduct(CameraRight, CameraOrigin);
			const float UpOffset = FVector::DotProduct(CameraUp, CameraOrigin);

			OutPlanes.Emplace(CameraRight, RightOffset + HalfWidth);
			OutPlanes.Emplace(-CameraRight, -RightOffset + HalfWidth);
			OutPlanes.Emplace(CameraUp, UpOffset + HalfHeight);
			OutPlanes.Emplace(-CameraUp, -UpOffset + HalfHeight);
			continue;
		}

		// 透视视锥：四个侧平面都过相机位置，法线朝视锥内侧，PlaneDot < 0 即在内侧。
		//
		// FOV 被解释成哪个轴并不固定：AspectRatioAxisConstraint 为 MaintainXFOV 时它是水平 FOV，
		// 为 MaintainYFOV（UE 默认）时它是垂直 FOV，而未设置时取引擎全局默认值 —— 在插件里猜错
		// 就会算出比真实视锥更窄的楔形，把仍然可见的源剔掉。这里不猜：两种解释各算一组半角，
		// 逐轴取较大者。它在 MaintainYFOV 下恰好等于真实视锥，在 MaintainXFOV 下只会比真实视锥
		// 更宽，所以无论全局默认是什么都不会误剔（代价最坏是宽屏下多留一点源）。
		const float TanHalfFOV = FMath::Tan(FMath::DegreesToRadians(POV.FOV * 0.5f));
		const float HalfFOVX = FMath::Atan(TanHalfFOV * FMath::Max(1.0f, Aspect));
		const float HalfFOVY = FMath::Atan(TanHalfFOV * FMath::Max(1.0f, 1.0f / Aspect));
		const float CosX = FMath::Cos(HalfFOVX);
		const float SinX = FMath::Sin(HalfFOVX);
		const float CosY = FMath::Cos(HalfFOVY);
		const float SinY = FMath::Sin(HalfFOVY);

		const FVector LeftNormal = (CameraForward * CosX + CameraRight * SinX).GetSafeNormal();
		const FVector RightNormal = (CameraForward * CosX - CameraRight * SinX).GetSafeNormal();
		const FVector TopNormal = (CameraForward * CosY + CameraUp * SinY).GetSafeNormal();
		const FVector BottomNormal = (CameraForward * CosY - CameraUp * SinY).GetSafeNormal();

		OutPlanes.Emplace(LeftNormal, FVector::DotProduct(LeftNormal, CameraOrigin));
		OutPlanes.Emplace(RightNormal, FVector::DotProduct(RightNormal, CameraOrigin));
		OutPlanes.Emplace(TopNormal, FVector::DotProduct(TopNormal, CameraOrigin));
		OutPlanes.Emplace(BottomNormal, FVector::DotProduct(BottomNormal, CameraOrigin));
	}

	return OutPlanes.Num() > 0;
}

bool AFogOfWar::IsVisionSourcePossiblyVisible(const TArray<FPlane>& InPlanes, const FVector& InCenter, float InRadiusCm)
{
	for (const FPlane& Plane : InPlanes)
	{
		// PlaneDot < 0 表示在视锥内侧。圆心到平面的有符号距离小于 -半径，说明整个圆都落在这个
		// 平面之外，它的并集不可能覆盖任何屏幕像素 —— 对屏幕空间的揭雾遮罩零贡献，可安全剔除。
		if (Plane.PlaneDot(InCenter) < -InRadiusCm)
		{
			return false;
		}
	}
	return true;
}

void AFogOfWar::UpdateSceneGpuVisionSources()
{
	const double TotalStartTime = FPlatformTime::Seconds();

	if (!SceneFogViewExtension.IsValid())
	{
		return;
	}

	const int32 SafeMaxSources = FMath::Max(1, MaxSceneGpuVisionSources);

	// 本帧的视野源清单：Reset 保留容量，Add 到 Num == 实际条数。交给渲染线程之后，下一帧会换回
	// 上一帧用过的缓冲，因此稳态下这里不产生任何分配。
	SceneGpuVisionSources.Reset();

	int32 VisitedCells = 0;
	int32 VisitedAgents = 0;

	UWorld* World = bEnableSceneGpuVisionSources ? GetWorld() : nullptr;
	UMassEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	UMassBattleHashGridSubsystem* HashGrid = World ? World->GetSubsystem<UMassBattleHashGridSubsystem>() : nullptr;
	if (!EntitySubsystem || !HashGrid)
	{
		// 收集不到（开关关闭 / 子系统缺失 / 世界正在销毁）时交出一份空表：渲染侧据此完全不注入
		// pass，雾直接消失，而不是停在上一帧的旧圆盘上。
		UploadSceneGpuVisionSources();
		return;
	}

	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();

	// 按当前观察队伍过滤视野源：只收集与本地玩家同队的单位视野，避免敌方单位周围也被揭雾。
	// 观察队伍来自外部注册的提供者（FFogOfWarViewingTeamProvider，本工程注册的是
	// UMassBattleGlobalVarFunctionLibrary::GetTeam）；提供者未注册或不可用时返回 INDEX_NONE，
	// 此时退化为不按队伍过滤（全场并集），避免整屏变黑。
	const int32 ViewingTeamIndex = FFogOfWarViewingTeamProvider::GetViewingTeam(this);
	const bool bFilterVisionSourcesByTeam = (ViewingTeamIndex != INDEX_NONE && ViewingTeamIndex != 127);
	if (!bFilterVisionSourcesByTeam)
	{
		static bool bWarnedMissingViewingTeam = false;
		if (!bWarnedMissingViewingTeam)
		{
			bWarnedMissingViewingTeam = true;
			UE_LOG(LogFogOfWar, Warning, TEXT("Viewing team is unavailable (no viewing team provider registered, or it returned INDEX_NONE); scene vision sources will not be filtered by team."));
		}
	}

	// 视图剔除平面每帧只构建一次，供全部视野源复用。开关关闭或相机不可用时平面为空，
	// IsVisionSourcePossiblyVisible 恒返回 true，收集循环里不需要再为开关分支。
	TArray<FPlane> ViewCullPlanes;
	const bool bUseViewCulling = bCullVisionSourcesOutOfView && BuildVisionSourceCullPlanes(ViewCullPlanes);
	int32 CulledSources = 0;
	int32 ContainedSources = 0;

	// 单格内的候选视野源：逐格复用同一块内存，避免每格一次分配。它只服务于"圆盘包含剔除"，
	// 通过剔除的候选才收进本帧清单。
	TArray<FVector4f> CellSources;

	const double CollectStartTime = FPlatformTime::Seconds();
	auto UploadCellVisionSources = [this, SafeMaxSources, ViewingTeamIndex, bFilterVisionSourcesByTeam, &EntityManager, &VisitedCells, &VisitedAgents, &ViewCullPlanes, bUseViewCulling, &CulledSources, &ContainedSources, &CellSources](const FHashGridAgentCell& Cell)
	{
		if (SceneGpuVisionSources.Num() >= SafeMaxSources)
		{
			return;
		}

		VisitedCells++;
		CellSources.Reset();
		for (const FAgentGridData& AgentData : Cell.Agents)
		{
			VisitedAgents++;

			// 本格已收下的候选 + 已收进清单的源，合起来不得超过容量。
			if (SceneGpuVisionSources.Num() + CellSources.Num() >= SafeMaxSources)
			{
				break;
			}

			// 视野源规则（有效性 + 队伍过滤 + 半径余量）统一收敛在 TryGetVisionSourceRadius 内，
			// 与 CPU 侧逐队收集共用同一份规则；这里只负责"筛选 + 写进 GPU 缓冲 + 计数"。
			// 观察队伍不可用（INDEX_NONE）时退化为不按队伍过滤（全场并集），避免整屏变黑。
			float UploadRadius = 0.0f;
			if (!TryGetVisionSourceRadius(
				EntityManager,
				AgentData,
				bFilterVisionSourcesByTeam ? ViewingTeamIndex : INDEX_NONE,
				SceneGpuVisionSourceRadiusPadding,
				UploadRadius))
			{
				continue;
			}

			const FVector WorldLocation = Cell.CellLocation + AgentData.GetRelativeLocation();

			// 视图剔除。必须排在"上限截断"之前：若先截断，完全落在屏幕外的源会先占满
			// MaxSceneGpuVisionSources 的名额，把屏幕内的源挤出去，画面直接丢视野。
			// 半径计入判定，是因为圆心在视锥外、圆本身仍压进屏幕的源依然有贡献。
			if (bUseViewCulling
				&& !IsVisionSourcePossiblyVisible(ViewCullPlanes, WorldLocation, UploadRadius + VisionSourceCullExtraMarginCm))
			{
				CulledSources++;
				continue;
			}

			const FVector4f Candidate(static_cast<float>(WorldLocation.X), static_cast<float>(WorldLocation.Y), UploadRadius, 0.0f);

			// 圆盘包含剔除（判据见 IsDiscContainedIn，集合等价、零画面误差）：
			// ① 本格已收下的某个候选包含本候选 → 本候选整条丢掉；
			// ② 本候选包含本格已收下的某个候选 → 把被包含的那条回收（它已无必要保留）。
			// 搜索范围就是同一个 HashGrid 格（250cm 量级），覆盖"单位挤在一起 / 单位贴着自己的建筑"
			// 这类最常见的重叠；跨格的大圆包小圆不会被这次剔除发现 —— 这不影响正确性（被删掉的源一定
			// 是冗余的），只影响收益上限。
			bool bContained = false;
			for (int32 KeptIndex = CellSources.Num() - 1; KeptIndex >= 0; --KeptIndex)
			{
				const FVector4f& Kept = CellSources[KeptIndex];
				if (IsDiscContainedIn(Candidate, Kept))
				{
					bContained = true;
					break;
				}
				if (IsDiscContainedIn(Kept, Candidate))
				{
					CellSources.RemoveAtSwap(KeptIndex);
				}
			}

			if (bContained)
			{
				ContainedSources++;
				continue;
			}

			CellSources.Add(Candidate);
		}

		// 通过两道剔除的格内候选按遍历顺序收进清单。顺序无要求：散射时重叠圆盘之间取并集，
		// 与先后无关。
		for (const FVector4f& Source : CellSources)
		{
			if (SceneGpuVisionSources.Num() >= SafeMaxSources)
			{
				break;
			}
			SceneGpuVisionSources.Add(Source);
		}
	};

	for (const TPair<FIntVector, TSharedPtr<FAgentGridBlock>>& BlockPair : HashGrid->AgentGrid)
	{
		if (SceneGpuVisionSources.Num() >= SafeMaxSources)
		{
			break;
		}
		if (!BlockPair.Value.IsValid())
		{
			continue;
		}

		const FAgentGridBlock& Block = *BlockPair.Value;
		for (TConstSetBitIterator<> CellIt(Block.OccupiedCells.OccupiedCellBitArray); CellIt; ++CellIt)
		{
			if (SceneGpuVisionSources.Num() >= SafeMaxSources)
			{
				break;
			}

			UploadCellVisionSources(Block.Cells[CellIt.GetIndex()]);
		}
	}
	const float CollectMs = static_cast<float>((FPlatformTime::Seconds() - CollectStartTime) * 1000.0);

	// 把清单交给渲染线程：一次加锁交换，游戏线程不碰任何图形资源。
	//（旧路径每帧在游戏线程重建一张 RHI 纹理并把 64KB 整块传上去，UploadMs 记的是那个代价。）
	//
	// 注意：UploadSceneGpuVisionSources 会把清单整个交换走（换回来的是上一帧的缓冲），
	// 因此"本帧源数"必须在交出之前先记下来，否则统计读到的是上一帧的条数。
	const int32 SourceCount = SceneGpuVisionSources.Num();

	const double UploadStartTime = FPlatformTime::Seconds();
	UploadSceneGpuVisionSources();
	const float UploadMs = static_cast<float>((FPlatformTime::Seconds() - UploadStartTime) * 1000.0);

	if (bEnableSceneGpuVisionPerformanceStats)
	{
		const float TotalMs = static_cast<float>((FPlatformTime::Seconds() - TotalStartTime) * 1000.0);
		RecordSceneGpuVisionPerfStats(TotalMs, CollectMs, UploadMs, VisitedCells, VisitedAgents, CulledSources, ContainedSources, SourceCount);
	}
}

void AFogOfWar::UploadSceneGpuVisionSources()
{
	if (!SceneFogViewExtension.IsValid())
	{
		return;
	}

	// 参数快照与视野源一起交给渲染线程：渲染线程只认这一份，既保证同帧一致，
	// 也让这些旋钮在 PIE 里改动能立刻生效（不必重启场景）。
	FFogOfWarSceneFogSettings Settings;
	Settings.EdgeWidthCm = FMath::Max(FogEdgeWidth, 0.0f);
	Settings.NotVisibleRegionBrightness = FMath::Clamp(NotVisibleRegionBrightness, 0.0f, 1.0f);
	Settings.PlaneZ = SceneFogWorldPlaneZ;
	Settings.MaskResolutionDivisor = FMath::Clamp(SceneGpuVisionMaskResolutionDivisor, 1, 4);

	// 清单本身会被交换走，换回来的是上一帧用过的缓冲（见调用方开头的 Reset）。
	SceneFogViewExtension->UploadFrameData_GameThread(SceneGpuVisionSources, Settings);
}

int32 AFogOfWar::CollectVisionSourcesByTeam(TArray<TArray<FFogVisionSource>>& OutSourcesByTeam, int32 InTeamCount, int32* OutHighestTeamIndexSeen) const
{
	const int32 SafeTeamCount = FMath::Max(0, InTeamCount);

	// 输出整体覆盖：桶数 = 队伍数（不足的队伍就是空桶，调用方据此跳过该队）。
	OutSourcesByTeam.Reset(SafeTeamCount);
	OutSourcesByTeam.SetNum(SafeTeamCount);
	if (OutHighestTeamIndexSeen)
	{
		*OutHighestTeamIndexSeen = INDEX_NONE;
	}

	UWorld* World = GetWorld();
	UMassEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	UMassBattleHashGridSubsystem* HashGrid = World ? World->GetSubsystem<UMassBattleHashGridSubsystem>() : nullptr;
	if (!EntitySubsystem || !HashGrid || SafeTeamCount <= 0)
	{
		return 0;
	}

	const FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();
	const int32 SafeMaxSources = FMath::Max(1, MaxSceneGpuVisionSources);

	int32 TotalSources = 0;
	for (const TPair<FIntVector, TSharedPtr<FAgentGridBlock>>& BlockPair : HashGrid->AgentGrid)
	{
		if (!BlockPair.Value.IsValid())
		{
			continue;
		}

		const FAgentGridBlock& Block = *BlockPair.Value;
		for (TConstSetBitIterator<> CellIt(Block.OccupiedCells.OccupiedCellBitArray); CellIt; ++CellIt)
		{
			const FHashGridAgentCell& Cell = Block.Cells[CellIt.GetIndex()];
			for (const FAgentGridData& AgentData : Cell.Agents)
			{
				// 先按"不过滤队伍"的规则判定，拿到该 Agent 的队伍下标后再决定落哪个桶：
				// 越界队伍因此也能被计入 OutHighestTeamIndexSeen（自检用），而不是被静默丢弃。
				int32 AgentTeamIndex = INDEX_NONE;
				float RadiusCm = 0.0f;
				if (!TryGetVisionSourceRadius(
					EntityManager,
					AgentData,
					INDEX_NONE,
					SceneGpuVisionSourceRadiusPadding,
					RadiusCm,
					&AgentTeamIndex))
				{
					continue;
				}

				if (OutHighestTeamIndexSeen && AgentTeamIndex > *OutHighestTeamIndexSeen)
				{
					*OutHighestTeamIndexSeen = AgentTeamIndex;
				}

				// 越界队伍 / 无队伍碎片：不落桶。
				if (!OutSourcesByTeam.IsValidIndex(AgentTeamIndex))
				{
					continue;
				}

				// 每队各自受 MaxSceneGpuVisionSources 上限约束（与 GPU 侧同一口径）。
				TArray<FFogVisionSource>& TeamSources = OutSourcesByTeam[AgentTeamIndex];
				if (TeamSources.Num() >= SafeMaxSources)
				{
					continue;
				}

				const FVector WorldLocation = Cell.CellLocation + AgentData.GetRelativeLocation();

				FFogVisionSource& Out = TeamSources.AddDefaulted_GetRef();
				Out.WorldLocation = FVector2D(WorldLocation.X, WorldLocation.Y);
				Out.RadiusCm = RadiusCm;
				++TotalSources;
			}
		}
	}

	return TotalSources;
}

void AFogOfWar::RecordSceneGpuVisionPerfStats(float TotalMs, float CollectMs, float UploadMs, int32 VisitedCells, int32 VisitedAgents, int32 CulledSources, int32 ContainedSources, int32 SourceCount)
{
	SceneGpuVisionPerfTotalMsAccum += TotalMs;
	SceneGpuVisionPerfCollectMsAccum += CollectMs;
	SceneGpuVisionPerfUploadMsAccum += UploadMs;
	// 用调用方在交出清单之前记下的条数：SceneGpuVisionSources 此刻已被交换成上一帧的缓冲。
	SceneGpuVisionPerfSourceCountAccum += SourceCount;
	SceneGpuVisionPerfVisitedCellsAccum += VisitedCells;
	SceneGpuVisionPerfVisitedAgentsAccum += VisitedAgents;
	SceneGpuVisionPerfCulledSourcesAccum += CulledSources;
	SceneGpuVisionPerfContainedSourcesAccum += ContainedSources;
	SceneGpuVisionPerfSampleCount++;

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double CurrentTime = World->GetTimeSeconds();
	if (CurrentTime - SceneGpuVisionPerfLastFlushTime >= SceneGpuVisionPerformanceLogInterval)
	{
		FlushSceneGpuVisionPerfStats(CurrentTime);
	}
}

void AFogOfWar::FlushSceneGpuVisionPerfStats(double CurrentTime)
{
	if (SceneGpuVisionPerfSampleCount <= 0)
	{
		return;
	}

	const float InvSamples = 1.0f / static_cast<float>(SceneGpuVisionPerfSampleCount);
	const FString CsvColumns = FString::Printf(
		TEXT("%d,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.1f"),
		SceneGpuVisionPerfSampleCount,
		SceneGpuVisionPerfTotalMsAccum * InvSamples,
		SceneGpuVisionPerfCollectMsAccum * InvSamples,
		SceneGpuVisionPerfUploadMsAccum * InvSamples,
		SceneGpuVisionPerfSourceCountAccum * InvSamples,
		SceneGpuVisionPerfVisitedCellsAccum * InvSamples,
		SceneGpuVisionPerfVisitedAgentsAccum * InvSamples,
		SceneGpuVisionPerfCulledSourcesAccum * InvSamples,
		SceneGpuVisionPerfContainedSourcesAccum * InvSamples);

	if (bLogSceneGpuVisionPerformanceToOutputLog)
	{
		UE_LOG(LogFogOfWar, Log, TEXT("[FogOfWarPerf][SceneGpuVisionAvg] %s"), *CsvColumns);
	}
	AppendSceneGpuVisionPerfCsvLine(CsvColumns);

	SceneGpuVisionPerfSampleCount = 0;
	SceneGpuVisionPerfTotalMsAccum = 0.0f;
	SceneGpuVisionPerfCollectMsAccum = 0.0f;
	SceneGpuVisionPerfUploadMsAccum = 0.0f;
	SceneGpuVisionPerfSourceCountAccum = 0;
	SceneGpuVisionPerfVisitedCellsAccum = 0;
	SceneGpuVisionPerfVisitedAgentsAccum = 0;
	SceneGpuVisionPerfCulledSourcesAccum = 0;
	SceneGpuVisionPerfLastFlushTime = CurrentTime;
}

void AFogOfWar::AppendSceneGpuVisionPerfCsvLine(const FString& CsvColumns) const
{
	if (!bWriteSceneGpuVisionPerformanceCsv || !GetWorld())
	{
		return;
	}

	const FString FilePath = FPaths::ProjectSavedDir() / Names::ScenePerformanceCsvRelativePath;
	const bool bNeedsHeader = !FPaths::FileExists(FilePath);
	FString Output;
	if (bNeedsHeader)
	{
		Output += TEXT("WorldTime,Channel,Samples,AvgTotalMs,AvgCollectMs,AvgUploadMs,AvgSourceCount,AvgVisitedCells,AvgVisitedAgents,AvgCulledSources,AvgContainedSources\n");
	}
	Output += FString::Printf(TEXT("%.3f,SceneGpuVisionAvg,%s\n"), GetWorld()->GetTimeSeconds(), *CsvColumns);
	FFileHelper::SaveStringToFile(Output, *FilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}
