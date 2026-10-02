// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWar.h"

#include "FogOfWarExploredLayerProvider.h"
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
	// 场景雾现在由 FFogOfWarSceneViewExtension 的三趟 pass 实现（世界空间散射 + R8G8 打包 + 合成），
	// 参数只有 C++ 属性这一个真值源，不再存在"材质资产里的默认值"与"C++ 每帧推送的值"
	// 两份可能分叉的契约（FOW_FogEdgeWidth 先前就是这样被漏掉的）。
}

namespace
{
	/**
	 * 视野场纹素数的每轴上限：约束场纹理的内存与散射遍历量。
	 * 地图范围异常大（或没配到真实地图范围、落到缺省 WorldGridSize）时按比例放大纹素边长，
	 * 而不是无限增大分辨率 —— 场变粗只是雾边变块状，而场爆表是直接分配失败。
	 */
	constexpr int32 VisionFieldMaxTexelsPerAxis = 2048;

	/**
	 * 由世界矩形与期望纹素边长算出视野场几何（本文件唯一实现）。
	 *
	 * 两轴共用一个缩放因子：分别 clamp 会让 X/Y 的纹素边长不一致，而着色器里的距离判定用的是
	 * 单一 FieldTexelSizeCm，非均匀的场会把视野圆算成椭圆。
	 * 纹素边长放大后重新按"世界尺寸 / 纹素边长"取整，保证场仍然铺满整个地图矩形
	 * （若直接沿用期望边长，被截断的场只会覆盖地图的一角）。
	 */
	FFogOfWarSceneVisionField ComputeSceneFogVisionField(
		const FVector2D& InWorldMin,
		const FVector2D& InWorldSize,
		float InDesiredTexelSizeCm)
	{
		FFogOfWarSceneVisionField Field;
		Field.WorldMin = InWorldMin;
		Field.WorldSize = InWorldSize;

		if (InWorldSize.X <= 0.0f || InWorldSize.Y <= 0.0f)
		{
			// 地图范围未知：交出一个无效几何，渲染侧据此完全不注入 pass（雾消失），
			// 而不是按 1×1 的场把整屏判成“从未探索”。
			return Field;
		}

		const float SafeDesiredTexelSizeCm = FMath::Max(InDesiredTexelSizeCm, 1.0f);
		const float DesiredX = InWorldSize.X / SafeDesiredTexelSizeCm;
		const float DesiredY = InWorldSize.Y / SafeDesiredTexelSizeCm;
		const float MaxDesired = FMath::Max(DesiredX, DesiredY);
		const float UniformScale = (MaxDesired > static_cast<float>(VisionFieldMaxTexelsPerAxis))
			? (static_cast<float>(VisionFieldMaxTexelsPerAxis) / MaxDesired)
			: 1.0f;

		Field.TexelSizeCm = SafeDesiredTexelSizeCm / UniformScale;
		Field.TexelCount = FIntPoint(
			FMath::Max(1, FMath::CeilToInt(static_cast<float>(InWorldSize.X) / Field.TexelSizeCm)),
			FMath::Max(1, FMath::CeilToInt(static_cast<float>(InWorldSize.Y) / Field.TexelSizeCm)));
		return Field;
	}

	/**
	 * 圆盘包含判定：InA 的圆盘是否完全落在 InB 的圆盘之内（等价于 dist(A,B) + rA <= rB）。
	 *
	 * 为什么删掉被包含的源是严格等价的：散射场取的是所有源的覆盖率最大值（整数原子取最大），A 若能揭开某个纹素，
	 * 则覆盖该纹素的 B 一定能揭开它、且覆盖率不低于 A。所以这是集合等价，不是近似 —— 与视图剔除
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
	 * 视野扇形解析（全插件唯一实现）：给出该 Agent 的揭雾扇形中轴与半角余弦。
	 *
	 * 返回 true = 需要按扇形裁剪（张角 ∈ (0, 360) 且能定位朝向）；false = 全向。
	 * 调用方在 false 时写"全向哨兵"（HalfAngleCos >= 1），下游据此整段跳过方向判定，
	 * 行为与旧的圆形揭雾逐字一致。
	 *
	 * 三个"退化即全向"的分支，理由各不相同：
	 *  ① 没有视野碎片 → 该实体根本不是视野源，由半径侧的同一个函数统一否掉，这里只管形状；
	 *  ② 张角 >= 360（或 <= 0）→ 张角语义上就是"全向"，不是缺失；
	 *  ③ 张角是扇形、但拿不到 FRotating（或方向为零）→ **宁可多揭也不要漏揭**：朝向缺失时
	 *     按全向处理，最坏是"雾少遮了一块"，而按 +X 处理会让单位的视野整块转到错误方向 ——
	 *     前者是保守的视觉误差，后者是把可见区域指错，代价不对等。
	 *
	 * 为什么朝向在这里现取、不缓存进 fragment：朝向每帧都在变（FRotating 由移动/朝向处理器写），
	 * 缓存进碎片等于引入一份注定过期的影子真值。张角则相反 —— 它来自索敌配置，由
	 * UMassBattleFogOfWarBootstrapProcessor 一次性写进 FMassVisionFragment::SightAngleDegrees。
	 *
	 * @param InVisionFragment 调用方已经取到的视野碎片（**不在这里重复取**：两条收集链都在
	 *                         每 Agent 的热路径上，多一次 GetFragmentDataPtr 就是一次多余的
	 *                         实体→块解析；传 nullptr 表示"没有视野碎片"，直接判全向）。
	 * @param OutForwardDir    扇形中轴（世界 XY 单位向量）；仅在返回 true 时有效。
	 * @param OutHalfAngleCos  cos(张角 / 2)；仅在返回 true 时有效。
	 */
	bool TryGetVisionSector(
		const FMassVisionFragment* InVisionFragment,
		const FMassEntityManager& InEntityManager,
		const FMassEntityHandle InEntity,
		FVector2D& OutForwardDir,
		float& OutHalfAngleCos)
	{
		if (!InVisionFragment
			|| !(InVisionFragment->SightAngleDegrees > 0.0f)
			|| InVisionFragment->SightAngleDegrees >= 360.0f)
		{
			return false;
		}

		// 朝向：必须**真的拿到**才敢按扇形裁剪（见上面 ③）。
		// 拿不到朝向（没有 FOW_ROTATION_FRAGMENT，或方向是零向量）时返回 false 退化成全向 ——
		// 不能"填一个默认 +X 再返回 true"：下游（GPU 着色器 / CPU 逐格内核）看到 cosHalfAngle < 1
		// 就会照做扇形裁剪，方向错 = 结果错，而且没有任何日志会提示。
		FVector2D Forward = FVector2D::ZeroVector;
		bool bHasForward = false;
#if FOW_HAS_MASSBATTLE_ROTATION
		if (const FOW_ROTATION_FRAGMENT* Rotation = InEntityManager.GetFragmentDataPtr<FOW_ROTATION_FRAGMENT>(InEntity))
		{
			const FVector RotationDir = FVector(FOW_GET_ROTATION_DIRECTION(*Rotation));
			if (RotationDir.SizeSquared2D() > KINDA_SMALL_NUMBER)
			{
				const FVector Normalized = RotationDir.GetSafeNormal2D();
				Forward = FVector2D(Normalized.X, Normalized.Y);
				bHasForward = true;
			}
		}
#endif

		if (!bHasForward)
		{
			return false;
		}

		OutForwardDir = Forward;
		OutHalfAngleCos = FMath::Cos(FMath::DegreesToRadians(InVisionFragment->SightAngleDegrees * 0.5f));
		return true;
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
	 * @param OutSightRadiusCm  可选输出：未加余量的原始视距（= FMassVisionFragment::SightRadius）。
	 *                          给需要"当前可见性判定"的消费者用：可见性必须按真实视距算，
	 *                          含渲染余量的半径只适合描述"被揭开的画面范围"。
	 * @param OutForwardDir     可选输出：扇形中轴（世界 XY 单位向量）。
	 * @param OutHalfAngleCos   可选输出：cos(张角 / 2)；**>= 1 表示全向**（哨兵值，见 FFogVisionSource）。
	 *                          两个扇形输出必须**成对请求**：只有请求了才多读一次朝向碎片 ——
	 *                          渲染热路径在不需要扇形时（例如将来某条只关心半径的消费者）
	 *                          不会为此付出朝向查询的代价。
	 * @return 是否为有效视野源。
	 */
	bool TryGetVisionSourceRadius(
		const FMassEntityManager& InEntityManager,
		const FAgentGridData& InAgentData,
		int32 InTeamIndex,
		float InRadiusPaddingCm,
		float& OutRadiusCm,
		int32* OutTeamIndex = nullptr,
		float* OutSightRadiusCm = nullptr,
		FVector2D* OutForwardDir = nullptr,
		float* OutHalfAngleCos = nullptr)
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
		//    原始视距单独回传：可见性判定要用它（见参数说明），此处不做任何加工，两个口径同源同一碎片。
		OutRadiusCm = VisionFragment->SightRadius + InRadiusPaddingCm;
		if (OutSightRadiusCm)
		{
			*OutSightRadiusCm = VisionFragment->SightRadius;
		}

		// ④ 揭雾形状（扇形 / 全向）—— 与半径同源同一碎片，解析逻辑见 TryGetVisionSector。
		//    哨兵约定：非扇形（含没有视野碎片、张角 >= 360）写 cosHalfAngle = 2（> 1），
		//    消费方据此整段跳过方向判定；朝向字段写成 +X 只是为了有个确定值，全向时它不被读。
		if (OutForwardDir || OutHalfAngleCos)
		{
			FVector2D ForwardDir(1.0, 0.0);
			float HalfAngleCos = 2.0f;
			TryGetVisionSector(VisionFragment, InEntityManager, InAgentData.EntityHandle, ForwardDir, HalfAngleCos);

			if (OutForwardDir)
			{
				*OutForwardDir = ForwardDir;
			}
			if (OutHalfAngleCos)
			{
				*OutHalfAngleCos = HalfAngleCos;
			}
		}
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

	// 新的扩展实例没有任何已探索层快照，必须把"上一次采用的版本号"一并作废：
	// Actor 在同一进程里被复用（换图重进 / PIE 重开）时，版本号若沿用旧值，
	// 下面那句"版本没变就不重新取"会让新扩展永远收不到已探索层，灰雾整场不出现。
	SceneFogExploredLayerSourceVersion = INDEX_NONE;

	// 场景雾的 GPU 实现是一个"世界作用域"的 SceneViewExtension：注册之后，该世界所有视图的
	// Tonemap 之后都会插入 FFogOfWarSceneViewExtension 的三趟 pass（散射场 + 打包 + 合成）。
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
	SceneGpuVisionSourceDirs.Reset();

	int32 VisitedCells = 0;
	int32 VisitedAgents = 0;

	UWorld* World = bEnableSceneGpuVisionSources ? GetWorld() : nullptr;
	UMassEntitySubsystem* EntitySubsystem = World ? World->GetSubsystem<UMassEntitySubsystem>() : nullptr;
	UMassBattleHashGridSubsystem* HashGrid = World ? World->GetSubsystem<UMassBattleHashGridSubsystem>() : nullptr;
	if (!EntitySubsystem || !HashGrid)
	{
		// 雾系统不可用（开关关闭 / 子系统缺失 / 世界正在销毁）：告诉渲染侧"本帧不遮蔽"，画面完全不受
		// 迷雾影响，而不是停在上一帧的旧圆盘上。
		// 注意这与"有雾、但本帧一条源都没收到"是两种不同语义：后者走下面的正常路径
		// （bSceneFogActive = true），由"覆盖率场全 0"表达成整屏遮蔽 —— 那才是正确的画面。
		UploadSceneGpuVisionSources(/*bSceneFogActive=*/false);
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
	// 通过剔除的候选才收进本帧清单。CellDirs 与 CellSources **同索引、同增删**：扇形参数必须
	// 跟着自己的源一起被包含剔除回收，否则两条数组会错位（源 A 用上源 B 的朝向）。
	TArray<FVector4f> CellSources;
	TArray<FVector4f> CellDirs;

	const double CollectStartTime = FPlatformTime::Seconds();
	auto UploadCellVisionSources = [this, SafeMaxSources, ViewingTeamIndex, bFilterVisionSourcesByTeam, &EntityManager, &VisitedCells, &VisitedAgents, &ViewCullPlanes, bUseViewCulling, &CulledSources, &ContainedSources, &CellSources, &CellDirs](const FHashGridAgentCell& Cell)
	{
		if (SceneGpuVisionSources.Num() >= SafeMaxSources)
		{
			return;
		}

		VisitedCells++;
		CellSources.Reset();
		CellDirs.Reset();  // 必须与 CellSources 同步清空：跨格残留会让扇形参数与源错位
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
			// 扇形参数与半径在同一次判定里取回：朝向碎片的读取与"要不要读"的决策都收在
			// TryGetVisionSourceRadius 内部，这里不再单独查一次碎片（见该函数的参数说明）。
			FVector2D SectorForward(1.0, 0.0);
			float SectorHalfAngleCos = 2.0f;
			if (!TryGetVisionSourceRadius(
				EntityManager,
				AgentData,
				bFilterVisionSourcesByTeam ? ViewingTeamIndex : INDEX_NONE,
				SceneGpuVisionSourceRadiusPadding,
				UploadRadius,
				/*OutTeamIndex=*/nullptr,
				/*OutSightRadiusCm=*/nullptr,
				&SectorForward,
				&SectorHalfAngleCos))
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

			// 扇形参数：张角与朝向由 TryGetVisionSourceRadius → TryGetVisionSector 一并解析
			// （与 CPU 侧探索层共用同一份"揭雾形状"实现，两条链路不会各判一套）。
			// 哨兵约定：HalfAngleCos >= 1 = 全向（张角 >= 360 / <= 0），写 (0, 0, 2) ——
			// 着色器看到 Dir.z >= 1 就整段跳过方向判定，行为与旧的圆形揭雾完全一致。
			const FVector4f CandidateDir = (SectorHalfAngleCos < 1.0f)
				? FVector4f(
					static_cast<float>(SectorForward.X), static_cast<float>(SectorForward.Y),
					SectorHalfAngleCos, 0.0f)
				: FVector4f(0.0f, 0.0f, 2.0f, 0.0f);

			// 圆盘包含剔除（判据见 IsDiscContainedIn，集合等价、零画面误差）：
			// ① 本格已收下的某个候选包含本候选 → 本候选整条丢掉；
			// ② 本候选包含本格已收下的某个候选 → 把被包含的那条回收（它已无必要保留）。
			// 搜索范围就是同一个 HashGrid 格（250cm 量级），覆盖"单位挤在一起 / 单位贴着自己的建筑"
			// 这类最常见的重叠；跨格的大圆包小圆不会被这次剔除发现 —— 这不影响正确性（被删掉的源一定
			// 是冗余的），只影响收益上限。
			// ⚠ 圆盘包含剔除对扇形源是**保守**的（圆 ⊇ 扇形，被删的源其扇形一定也被保留下来的圆盖住），
			//    但扇形的"方向"信息不能丢：所以 CellDirs 必须与 CellSources 同索引增删。
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
					CellDirs.RemoveAtSwap(KeptIndex);
				}
			}

			if (bContained)
			{
				ContainedSources++;
				continue;
			}

			CellSources.Add(Candidate);
			CellDirs.Add(CandidateDir);
		}

		// 通过两道剔除的格内候选按遍历顺序收进清单。顺序无要求：散射时重叠圆盘之间取并集，
		// 与先后无关。两条数组必须同时追加，保持同索引。
		for (int32 SourceIndex = 0; SourceIndex < CellSources.Num(); ++SourceIndex)
		{
			if (SceneGpuVisionSources.Num() >= SafeMaxSources)
			{
				break;
			}
			SceneGpuVisionSources.Add(CellSources[SourceIndex]);
			SceneGpuVisionSourceDirs.Add(CellDirs[SourceIndex]);
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
	// 走到这里说明雾系统本身是好的（开关开着、子系统存在），因此本帧一律要遮蔽画面：
	// 即使一条视野源都没收到（相机移出所有单位的视野范围，或源被视图剔除干净），
	// 也应该表现为整屏迷雾，而不是迷雾消失。
	UploadSceneGpuVisionSources(/*bSceneFogActive=*/true);
	const float UploadMs = static_cast<float>((FPlatformTime::Seconds() - UploadStartTime) * 1000.0);

	if (bEnableSceneGpuVisionPerformanceStats)
	{
		const float TotalMs = static_cast<float>((FPlatformTime::Seconds() - TotalStartTime) * 1000.0);
		RecordSceneGpuVisionPerfStats(TotalMs, CollectMs, UploadMs, VisitedCells, VisitedAgents, CulledSources, ContainedSources, SourceCount);
	}
}

void AFogOfWar::UploadSceneGpuVisionSources(bool bSceneFogActive)
{
	if (!SceneFogViewExtension.IsValid())
	{
		return;
	}

	// 观察队伍：既决定视野源过滤（收集阶段已经用过一次），也决定向提供者要哪一支队伍的探索层。
	const int32 ViewingTeamIndex = FFogOfWarViewingTeamProvider::GetViewingTeam(this);

	// 场几何与已探索层共用同一次查询：提供者给出的地图矩形就是逐格探索层的矩形，
	// 而视野场必须与它同矩形（G 通道直接沿用场的 UV），否则灰雾会整体错位。
	// 拿不到提供者时退回本 Actor 自己的网格范围（WorldGridSize 派生的那份），此时只有二态雾。
	FVector2D FieldWorldMin = GridBottomLeftWorldLocation;
	FVector2D FieldWorldSize = GridSize;

	FFogOfWarExploredLayerView ExploredView;
	if (FFogOfWarExploredLayerProvider::GetExploredLayer(ViewingTeamIndex, ExploredView))
	{
		FieldWorldMin = ExploredView.WorldMin;
		FieldWorldSize = ExploredView.WorldSize;

		// 版本号不变就不重新取：探索层每秒只变几次，而这里是每帧一次。
		if (ExploredView.Version != SceneFogExploredLayerSourceVersion)
		{
			const int64 CellCount = static_cast<int64>(ExploredView.Width) * static_cast<int64>(ExploredView.Height);
			const int32 ByteCount = static_cast<int32>((CellCount + 7) / 8);

			// 位打包布局与外部位图完全一致（都是 LSB-first 位流），所以这里只是一次 memcpy：
			// 不需要在 CPU 端展开成逐格字节，也就不需要为展开准备任何中间缓冲。
			// 先清零再拷——位图的字节数不一定是 4 的倍数，最后一个 word 的高位必须保持 0，
			// 否则那几个位会被着色器当成“已探索”。
			FFogOfWarSceneExploredLayer ExploredLayer;
			ExploredLayer.PackedBits.SetNumZeroed(static_cast<int32>((CellCount + 31) / 32));
			FMemory::Memcpy(ExploredLayer.PackedBits.GetData(), ExploredView.Bitmap, ByteCount);
			ExploredLayer.Extent = FIntPoint(ExploredView.Width, ExploredView.Height);
			ExploredLayer.Version = ExploredView.Version;

			SceneFogExploredLayerSourceVersion = ExploredView.Version;
			SceneFogViewExtension->UploadExploredLayer_GameThread(ExploredLayer);
		}
	}
	else if (SceneFogExploredLayerSourceVersion != INDEX_NONE)
	{
		// 提供者不可用（未注册 / 网格未就绪 / 观察队伍不可用）：主动清掉灰雾让画面回到二态，
		// 而不是继续展示上一次的快照 —— 那份数据可能属于另一张地图或另一支队伍。
		SceneFogExploredLayerSourceVersion = INDEX_NONE;
		SceneFogViewExtension->UploadExploredLayer_GameThread(FFogOfWarSceneExploredLayer());
	}

	// 参数快照与视野源一起交给渲染线程：渲染线程只认这一份，既保证同帧一致，
	// 也让这些旋钮在 PIE 里改动能立刻生效（不必重启场景）。
	FFogOfWarSceneFogSettings Settings;
	Settings.EdgeWidthCm = FMath::Max(FogEdgeWidth, 0.0f);
	Settings.NotVisibleRegionBrightness = FMath::Clamp(NotVisibleRegionBrightness, 0.0f, 1.0f);
	Settings.ExploredRegionBrightness = FMath::Clamp(ExploredRegionBrightness, 0.0f, 1.0f);
	Settings.Opacity = FMath::Clamp(SceneFogOpacity, 0.0f, 1.0f);
	Settings.PlaneZ = SceneFogWorldPlaneZ;

	// 清单本身会被交换走，换回来的是上一帧用过的缓冲（见调用方开头的 Reset）。
	SceneFogViewExtension->UploadFrameData_GameThread(
		SceneGpuVisionSources,
		SceneGpuVisionSourceDirs,
		Settings,
		ComputeSceneFogVisionField(FieldWorldMin, FieldWorldSize, VisionFieldTexelSizeCm),
		bSceneFogActive);
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

	// 每队上限：0 = 不限（默认）。**故意不与渲染侧的 MaxSceneGpuVisionSources 共用** ——
	// 那条是渲染预算的保险丝（且 GPU 侧截断前已做过视图剔除 + 圆盘包含剔除），而本函数服务的是
	// "该队所有视野源的并集"这条逻辑语义，CPU 侧没有任何可用的剔除依据（探索层是全图历史，
	// 不能按本机视口裁剪），所以任何截断都只会让探索层凭空缺一块。
	const int32 TeamSourceLimit = FMath::Max(0, MaxCpuVisionSourcesPerTeam);

	// ★【诊断】被上限丢弃的源数。只在显式配了 CPU 侧保险丝（TeamSourceLimit > 0）时才可能非零：
	//   - 丢掉哪些取决于 HashGrid 遍历顺序 → 同一局面两次运行的探索层可能不同（不可复现）；
	//   - 视距较小时（圆盘远小于地图）会真的缺掉一整支部队覆盖的区域，而画面/日志上都看不出来。
	// 把"丢了多少"记下来并限频告警，避免这种精度损失被性能数字掩盖。
	int32 TruncatedSources = 0;

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
				float SightRadiusCm = 0.0f;
				FVector2D SectorForward(1.0, 0.0);
				float SectorHalfAngleCos = 2.0f;
				if (!TryGetVisionSourceRadius(
					EntityManager,
					AgentData,
					INDEX_NONE,
					SceneGpuVisionSourceRadiusPadding,
					RadiusCm,
					&AgentTeamIndex,
					&SightRadiusCm,
					&SectorForward,
					&SectorHalfAngleCos))
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

				// 每队各自受 MaxCpuVisionSourcesPerTeam 约束；默认 0 = 不限，因此常态下这里不会截断。
				TArray<FFogVisionSource>& TeamSources = OutSourcesByTeam[AgentTeamIndex];
				if (TeamSourceLimit > 0 && TeamSources.Num() >= TeamSourceLimit)
				{
					++TruncatedSources; // ★【诊断】本队名额已满（仅在显式设了 CPU 侧保险丝时可能发生）
					continue;
				}

				const FVector WorldLocation = Cell.CellLocation + AgentData.GetRelativeLocation();

				FFogVisionSource& Out = TeamSources.AddDefaulted_GetRef();
				Out.WorldLocation = FVector2D(WorldLocation.X, WorldLocation.Y);
				Out.RadiusCm = RadiusCm;
				Out.SightRadiusCm = SightRadiusCm;
				// 揭雾形状随源一起交给 CPU 消费者（探索层 / 可见层都要按同一形状裁剪，
				// 否则画面上看不见的地方会被 CPU 侧标成"已探索 / 可见"）。
				Out.ForwardDir = SectorForward;
				Out.HalfAngleCos = SectorHalfAngleCos;
				++TotalSources;
			}
		}
	}

	// ★【诊断】截断告警（限频 10s，避免每次刷新都刷屏）：把"静默丢数据"变成一条明确记录。
	// 本函数在刷新路径上每次调用一次（本工程约每 2s），限频后最多每 10s 一条。
	if (TruncatedSources > 0)
	{
		static double LastTruncWarnTime = -1.0;
		const double NowSeconds = World ? World->GetTimeSeconds() : 0.0;
		if (LastTruncWarnTime < 0.0 || (NowSeconds - LastTruncWarnTime) >= 10.0)
		{
			LastTruncWarnTime = NowSeconds;
			UE_LOG(LogFogOfWar, Warning,
				TEXT("[诊断] 视野源被每队上限截断 %d 条（MaxCpuVisionSourcesPerTeam=%d，该队名额已满，按 HashGrid 遍历顺序丢弃靠后的单位）。")
				TEXT("被丢源的视野不进入当前可见层与已探索层：探索层结果因此取决于遍历顺序（不可复现），")
				TEXT("视距远小于地图时还会真的缺掉一整片区域。这是显式设置的 CPU 侧保险丝所致，")
				TEXT("默认 0（不限）不会有此问题；要恢复完整探索请把它调回 0 或调到不小于同队最大单位数。"),
				TruncatedSources, TeamSourceLimit);
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
