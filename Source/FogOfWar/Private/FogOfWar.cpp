// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWar.h"

#include "FogOfWarMassBinding.h"
#include "Components/PostProcessComponent.h"
#include "Engine/Texture2D.h"
#include "MassEntitySubsystem.h"
#include "Subsystems/MassBattleHashGridSubsystem.h"
#include "Subsystems/MinimapDataSubsystem.h"
#include "VarSystem/MassBattleGlobalVarFunctionLibrary.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogFogOfWar);

DECLARE_STATS_GROUP(TEXT("FogOfWar"), STATGROUP_FogOfWar, STATCAT_Advanced);

namespace Names
{
	const TCHAR* ScenePerformanceCsvRelativePath = TEXT("Logs/FogOfWar_ScenePerf.csv");

	const FName FOW_NotVisibleRegionBrightness("FOW_NotVisibleRegionBrightness");
	const FName FOW_BottomLeftWorldLocation("FOW_BottomLeftWorldLocation");
	const FName FOW_GridSize("FOW_GridSize");
	const FName FOW_GridWorldSize("FOW_GridWorldSize");
	const FName FOW_SceneGpuVisionSourceTexture("FOW_SceneGpuVisionSourceTexture");
	const FName FOW_SceneGpuVisionSourceCount("FOW_SceneGpuVisionSourceCount");
	const FName FOW_EnableSceneGpuVisionSources("FOW_EnableSceneGpuVisionSources");

	UTexture2D* CreateSceneGpuVisionDataTexture(UObject* Outer, int32 Width)
	{
		if (!Outer || Width <= 0)
		{
			return nullptr;
		}

		UTexture2D* Texture = UTexture2D::CreateTransient(Width, 1, PF_A32B32G32R32F, TEXT("SceneGpuVisionSourceTexture"));
		if (!Texture)
		{
			return nullptr;
		}

		Texture->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
		Texture->SRGB = 0;
		Texture->Filter = TF_Nearest;
		Texture->UpdateResource();
		return Texture;
	}

}

namespace
{
	/**
	 * 视野源判定（全插件唯一实现）：该 Agent 是否为指定队伍的视野源，并给出生效揭雾半径。
	 * GPU 揭雾收集（UpdateSceneGpuVisionSourceTexture）与 CPU 侧逐队收集（CollectVisionSourcesByTeam）
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

		// ② 队伍过滤：FTeam::index 与 UMassBattleGlobalVarFunctionLibrary::GetTeam 同口径。
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

		// ③ 生效半径 = 单体视距 + 场景揭雾余量（与上传给 GPU 的口径一致，避免探索层比画面小一圈）。
		OutRadiusCm = VisionFragment->SightRadius + InRadiusPaddingCm;
		return true;
	}
}
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcessComponent"));
	PostProcess->SetupAttachment(RootComponent);
}

void AFogOfWar::SetCommonMIDParameters(UMaterialInstanceDynamic* MID)
{
	if (!MID)
	{
		return;
	}

	MID->SetVectorParameterValue(Names::FOW_BottomLeftWorldLocation, FVector(GridBottomLeftWorldLocation.X, GridBottomLeftWorldLocation.Y, 0));
	MID->SetVectorParameterValue(Names::FOW_GridSize, FVector(GridSize.X, GridSize.Y, 0));
	MID->SetVectorParameterValue(Names::FOW_GridWorldSize, FVector(GridSize.X, GridSize.Y, 0));
	MID->SetScalarParameterValue(Names::FOW_NotVisibleRegionBrightness, NotVisibleRegionBrightness);
	MID->SetScalarParameterValue(Names::FOW_EnableSceneGpuVisionSources, bEnableSceneGpuVisionSources ? 1.0f : 0.0f);
	MID->SetScalarParameterValue(Names::FOW_SceneGpuVisionSourceCount, static_cast<float>(SceneGpuVisionSourceCount));
	if (SceneGpuVisionSourceTexture)
	{
		MID->SetTextureParameterValue(Names::FOW_SceneGpuVisionSourceTexture, SceneGpuVisionSourceTexture);
	}
}

void AFogOfWar::Activate()
{
	if (!ensure(!bActivated))
	{
		return;
	}
	bActivated = true;

	checkf(IsValid(PostProcessingMaterial), TEXT("PostProcessingMaterial must be set. GPU FogOfWar uses a single post-process material."));

	Initialize();

	UMinimapDataSubsystem* MinimapSubsystem = UMinimapDataSubsystem::Get();
	if (MinimapSubsystem)
	{
		MinimapSubsystem->SetVisionGridActive(false);
	}

	SceneGpuVisionSourceTexture = Names::CreateSceneGpuVisionDataTexture(this, FMath::Max(1, MaxSceneGpuVisionSources));
	PostProcessingMID = UMaterialInstanceDynamic::Create(PostProcessingMaterial, this);
	SetCommonMIDParameters(PostProcessingMID);
	if (SceneGpuVisionSourceTexture)
	{
		PostProcessingMID->SetTextureParameterValue(Names::FOW_SceneGpuVisionSourceTexture, SceneGpuVisionSourceTexture);
	}

	PostProcess->AddOrUpdateBlendable(PostProcessingMID);

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

	UpdateSceneGpuVisionSourceTexture();
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

void AFogOfWar::UpdateSceneGpuVisionSourceTexture()
{
	const double TotalStartTime = FPlatformTime::Seconds();
	if (!PostProcessingMID)
	{
		return;
	}

	PostProcessingMID->SetScalarParameterValue(Names::FOW_EnableSceneGpuVisionSources, bEnableSceneGpuVisionSources ? 1.0f : 0.0f);
	if (!bEnableSceneGpuVisionSources)
	{
		PostProcessingMID->SetScalarParameterValue(Names::FOW_SceneGpuVisionSourceCount, 0.0f);
		SceneGpuVisionSourceCount = 0;
		return;
	}

	if (!SceneGpuVisionSourceTexture || SceneGpuVisionSourceTexture->GetSizeX() != FMath::Max(1, MaxSceneGpuVisionSources))
	{
		SceneGpuVisionSourceTexture = Names::CreateSceneGpuVisionDataTexture(this, FMath::Max(1, MaxSceneGpuVisionSources));
		if (SceneGpuVisionSourceTexture)
		{
			PostProcessingMID->SetTextureParameterValue(Names::FOW_SceneGpuVisionSourceTexture, SceneGpuVisionSourceTexture);
		}
	}
	if (!SceneGpuVisionSourceTexture)
	{
		return;
	}

	UWorld* World = GetWorld();
	UMassEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	UMassBattleHashGridSubsystem* HashGrid = World ? World->GetSubsystem<UMassBattleHashGridSubsystem>() : nullptr;
	if (!EntitySubsystem || !HashGrid)
	{
		return;
	}

	FMassEntityManager& EntityManager = EntitySubsystem->GetMutableEntityManager();

	const int32 SafeMaxSources = FMath::Max(1, MaxSceneGpuVisionSources);
	SceneGpuVisionSourceDataBuffer.SetNumZeroed(SafeMaxSources);
	SceneGpuVisionSourceCount = 0;
	int32 VisitedCells = 0;
	int32 VisitedAgents = 0;

	// 按当前观察队伍过滤视野源：只收集与本地玩家同队的单位视野，避免敌方单位周围也被揭雾。
	// GetTeam() 在全局变量子系统不可用时返回 INDEX_NONE，此时退化为不按队伍过滤（全场并集），避免整屏变黑。
	const int32 ViewingTeamIndex = UMassBattleGlobalVarFunctionLibrary::GetTeam(this);
	const bool bFilterVisionSourcesByTeam = (ViewingTeamIndex != INDEX_NONE);
	if (!bFilterVisionSourcesByTeam)
	{
		static bool bWarnedMissingViewingTeam = false;
		if (!bWarnedMissingViewingTeam)
		{
			bWarnedMissingViewingTeam = true;
			UE_LOG(LogFogOfWar, Warning, TEXT("Viewing team is unavailable (GetTeam returned INDEX_NONE); scene vision sources will not be filtered by team."));
		}
	}

	const double CollectStartTime = FPlatformTime::Seconds();
	auto UploadCellVisionSources = [this, SafeMaxSources, ViewingTeamIndex, bFilterVisionSourcesByTeam, &EntityManager, &VisitedCells, &VisitedAgents](const FHashGridAgentCell& Cell)
	{
		if (SceneGpuVisionSourceCount >= SafeMaxSources)
		{
			return;
		}

		VisitedCells++;
		for (const FAgentGridData& AgentData : Cell.Agents)
		{
			VisitedAgents++;
			if (SceneGpuVisionSourceCount >= SafeMaxSources)
			{
				break;
			}

			// 视野源规则（有效性 + 队伍过滤 + 半径余量）统一收敛在 TryGetVisionSourceRadius 内，
			// 与 CPU 侧逐队收集共用同一份规则；这里只负责"写进 GPU 缓冲 + 计数"。
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

			SceneGpuVisionSourceDataBuffer[SceneGpuVisionSourceCount] = FLinearColor(WorldLocation.X, WorldLocation.Y, UploadRadius, 0.0f);
			SceneGpuVisionSourceCount++;
		}
	};

	for (const TPair<FIntVector, TSharedPtr<FAgentGridBlock>>& BlockPair : HashGrid->AgentGrid)
	{
		if (SceneGpuVisionSourceCount >= SafeMaxSources)
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
			if (SceneGpuVisionSourceCount >= SafeMaxSources)
			{
				break;
			}

			UploadCellVisionSources(Block.Cells[CellIt.GetIndex()]);
		}
	}
	const float CollectMs = static_cast<float>((FPlatformTime::Seconds() - CollectStartTime) * 1000.0);

	const double UploadStartTime = FPlatformTime::Seconds();
	FTexture2DMipMap& Mip = SceneGpuVisionSourceTexture->GetPlatformData()->Mips[0];
	void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, SceneGpuVisionSourceDataBuffer.GetData(), sizeof(FLinearColor) * SafeMaxSources);
	Mip.BulkData.Unlock();
	SceneGpuVisionSourceTexture->UpdateResource();
	const float UploadMs = static_cast<float>((FPlatformTime::Seconds() - UploadStartTime) * 1000.0);

	PostProcessingMID->SetScalarParameterValue(Names::FOW_SceneGpuVisionSourceCount, static_cast<float>(SceneGpuVisionSourceCount));
	PostProcessingMID->SetTextureParameterValue(Names::FOW_SceneGpuVisionSourceTexture, SceneGpuVisionSourceTexture);

	if (bEnableSceneGpuVisionPerformanceStats)
	{
		const float TotalMs = static_cast<float>((FPlatformTime::Seconds() - TotalStartTime) * 1000.0);
		RecordSceneGpuVisionPerfStats(TotalMs, CollectMs, UploadMs, VisitedCells, VisitedAgents);
	}
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

void AFogOfWar::RecordSceneGpuVisionPerfStats(float TotalMs, float CollectMs, float UploadMs, int32 VisitedCells, int32 VisitedAgents)
{
	SceneGpuVisionPerfTotalMsAccum += TotalMs;
	SceneGpuVisionPerfCollectMsAccum += CollectMs;
	SceneGpuVisionPerfUploadMsAccum += UploadMs;
	SceneGpuVisionPerfSourceCountAccum += SceneGpuVisionSourceCount;
	SceneGpuVisionPerfVisitedCellsAccum += VisitedCells;
	SceneGpuVisionPerfVisitedAgentsAccum += VisitedAgents;
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
		TEXT("%d,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f"),
		SceneGpuVisionPerfSampleCount,
		SceneGpuVisionPerfTotalMsAccum * InvSamples,
		SceneGpuVisionPerfCollectMsAccum * InvSamples,
		SceneGpuVisionPerfUploadMsAccum * InvSamples,
		SceneGpuVisionPerfSourceCountAccum * InvSamples,
		SceneGpuVisionPerfVisitedCellsAccum * InvSamples,
		SceneGpuVisionPerfVisitedAgentsAccum * InvSamples);

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
		Output += TEXT("WorldTime,Channel,Samples,AvgTotalMs,AvgCollectMs,AvgUploadMs,AvgSourceCount,AvgVisitedCells,AvgVisitedAgents\n");
	}
	Output += FString::Printf(TEXT("%.3f,SceneGpuVisionAvg,%s\n"), GetWorld()->GetTimeSeconds(), *CsvColumns);
	FFileHelper::SaveStringToFile(Output, *FilePath, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
}
