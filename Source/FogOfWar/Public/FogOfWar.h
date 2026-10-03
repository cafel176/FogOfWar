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

/** 视野场的持久载体（见 AFogOfWar::VisionFieldTexture）。 */
class UTextureRenderTarget2D;

/**
 * @struct FFogVisionSource
 * @brief CPU 侧一条视野源：揭雾中心（世界 XY）+ 生效半径（cm）+ 揭雾扇形（方向 + 半角余弦）。
 * @details 与交给场景雾渲染的是同一批数据：已按观察队伍过滤，且半径已叠加
 *          AFogOfWar::SceneGpuVisionSourceRadiusPadding（即实际被揭开的范围）。
 *          供同工程的 CPU 消费者（如地图的"已探索"层）复用同一条收集链，
 *          避免各自复刻"谁能看见"的规则而产生分叉。
 *
 *          形状（圆 / 扇形）同样来自这条链、同样与 GPU 一致：**CPU 侧必须按扇形裁剪**，
 *          否则"屏幕上看不见的地方，小地图/探索层却标成已探索"，两套视野说法互相打架。
 *          见 HalfAngleCos 的哨兵约定。
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

	/// @brief 揭雾扇形的中轴方向（世界 XY 单位向量）。仅当 HalfAngleCos < 1 时有意义。
	FVector2D ForwardDir = FVector2D(1.0f, 0.0f);

	/// @brief 揭雾扇形半角的余弦 = cos(张角 / 2)；**>= 1 = 全向**（哨兵，默认值即此）。
	/// @details 判据与 GPU 着色器、以及 CPU 逐格内核完全一致：落在扇形外的格/纹素**完全不揭雾**
	///          （硬边，只在半径方向软化）。消费方看到 >= 1 必须**整段跳过**方向判定 ——
	///          这是"张角 >= 360 / <= 0 / 拿不到朝向"三种情况的统一表达，跳过后行为与旧的圆形揭雾逐字一致。
	///          存余弦而不是角度：两侧的判定都在逐格/逐纹素的最内层循环里（地图探索层是千万格量级），
	///          每格一次 acos 是纯粹的浪费；一次开方 + 一次点积就够。角度只在需要人来读日志时才算。
	float HalfAngleCos = 1.0f;
};

/// 声明一个全局的日志分类，用于本模块的日志输出
DECLARE_LOG_CATEGORY_EXTERN(LogFogOfWar, Log, All)

/**
 * @class AFogOfWar
 * @brief 战争迷雾系统的核心管理器Actor。
 * @details 场景战争迷雾的 GPU 路径：CPU 从 MassBattle HashGrid 收集带 FMassVisionFragment 的
 * Agent 视野源（全图遍历、按观察队伍过滤、视图剔除 + 圆盘包含剔除，受 MaxSceneGpuVisionSources
 * 上限约束），把 (WorldX, WorldY, SightRadius + SceneGpuVisionSourceRadiusPadding) 连同**揭雾形状**
 * （全向 / 扇形：中轴 + 半角余弦）以及视野场几何交给 FFogOfWarSceneViewExtension，
 * 由它在 Tonemap 之后散射成世界空间视野场再合成（详见该类的说明）。
 * 同一批视野源也按队伍暴露给 CPU 侧消费者（CollectVisionSourcesByTeam，供探索层等逻辑累积历史），
 * 且**形状一并带过去** —— 屏幕上的雾与 CPU 侧的探索层必须是同一个形状，否则两套"能不能看见"互相打架。
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

	/**
	 * @brief 视野场的持久纹理（PF_R8G8：R = 当前视野覆盖率，G = 已探索）；未激活 / 尺寸未知时为 nullptr。
	 * @details 供调试可视化与新对局自检用。**重要**：这张纹理在渲染线程被每帧写入，游戏线程直接
	 *          采样它（例如建 MID 去读）会撞上未完成的写入 —— 要读它就必须回到渲染线程里读
	 *          （合成趟就是这么做的）。
	 */
	UTextureRenderTarget2D* GetVisionFieldTexture() const { return VisionFieldTexture; }

	/**
	 * @brief Actor 结束时撤下向其余系统发布的状态。
	 * @details 必须撤：消费方（Agent 渲染处理器 / 音频门控）每帧都读这个快照，Actor 没了却还留着
	 *          "bEnabled = true"，它们就会继续按旧的可见性去遮敌人。
	 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

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

	/// @brief 场景雾的 GPU 揭雾源总开关。关闭时本帧告诉渲染侧“不遮蔽”，连 pass 都不注入（雾消失）。
	/// @note 与之相对：开关开着、但本帧一条视野源都没有时，画面是**整屏遮蔽**（什么都看不见），
	///       而不是雾消失 —— 这两件事在渲染侧由独立的标志区分，见 UploadSceneGpuVisionSources 的说明。
	/// @note 它只是**三道**"不画雾"闸门里的一道，另外两道（全局游戏状态不允许 / 观察者是观察者·管理员）
	///       与本开关相互独立、各自成立（见 UpdateSceneGpuVisionSources 的说明）。
	///       所以"雾没画出来"有三个来源，排查时先确认是哪一个 —— 每一道都会打一条一次性日志。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU")
	bool bEnableSceneGpuVisionSources = true;

	/// @brief 每帧最多交给 GPU 的视野源数量，超出时按 HashGrid 遍历顺序截断。
	/// @note 新实现里这个上限只决定"每帧散射多少个圆盘"，不再像旧材质那样直接决定内层循环次数，
	///       因此它从"性能生死线"降级为"异常情况下的保险丝"。
	///       它只约束**渲染侧**：CPU 侧逐队收集（探索层累积）另有一条独立上限 MaxCpuVisionSourcesPerTeam，
	///       两者性质完全不同（一个是渲染预算，一个是逻辑语义），**不要**为了让探索层"多看见一点"来调这条。
	///       默认 8192：本工程实测单队可达 6000+ 单位，4096 会让每个队伍持续丢掉一部分单位的视野。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "1", UIMin = "1"))
	int32 MaxSceneGpuVisionSources = 8192;

	/// @brief 每队交给 **CPU 侧消费者**（探索层累积）的视野源上限；0 = 不限（默认）。
	/// @details 与 MaxSceneGpuVisionSources 是两件事，故意不共用：
	///          - 那条是**渲染预算**的保险丝，且 GPU 侧在截断前已经做了视图剔除 + 格内圆盘包含剔除；
	///          - 本属性约束的是**逻辑语义** —— CPU 探索层的定义就是"该队所有视野源的并集"，
	///            一旦截断，同队就有一部分单位的视野凭空消失，且丢谁取决于 HashGrid 遍历顺序
	///            （同一局面两次运行可能不同，不可复现）。CPU 侧也没有可用的剔除依据：
	///            探索层是全图历史，不能按本机视口/相机距离裁剪。
	///          因此默认 0（不限）。确需为极端规模设保险丝时，它只是"性能兜底"，设小了直接损失探索精度
	///          （触发时会有限频告警，见 .cpp 内的 CollectVisionSourcesByTeam）。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|CPU", meta = (ClampMin = "0", UIMin = "0"))
	int32 MaxCpuVisionSourcesPerTeam = 0;

	/// @brief 交给 GPU 的每个视野源额外半径。用于抵消 hash/cell/投影边缘误差，避免漏视野。
	/// @details 这个余量同时吸收了"视野圆按世界 Z 平面投影、而像素所在表面可能高出一截"带来的
	///          屏幕偏移（见 SceneFogWorldPlaneZ），因此不建议调到 0。
	UPROPERTY(EditAnywhere, Category = "FogOfWar|Scene GPU", meta = (ClampMin = "0.0", UIMin = "0.0"))
	float SceneGpuVisionSourceRadiusPadding = 300.0f;

	/// @brief 视野边缘的软化宽度（cm）。0 = 硬边。
	/// @details 覆盖率 = 1 - smoothstep(Radius - FogEdgeWidth, Radius, Distance)，范围内 1、外 0。
	///          每帧随参数快照交给渲染线程，因此在 PIE 里实时可调。
	/// @note 本参数只软化**半径方向**的边界。扇形源的两条直边（张角边界）是硬边、不受它影响 ——
	///       直边的软化需要在角度域上做一次 smoothstep，而扇形在 Mass 单位上本就是"索敌范围的近似表达"，
	///       多这一档软化只是把边界挪几度，收益远小于它引入的额外分支。硬边与索敌的判定边界因此完全重合。
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
	/// @note 相机移出所有单位的视野范围时，本帧的源会被清空，此时画面整屏被迷雾遮蔽 ——
	///       这是“什么都看不见”的正确表现，不是异常。
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
	 *          UMassBattleGlobalVarFunctionLibrary::GetTeam）。
	 *
	 *          三道"本帧完全不遮蔽"的闸门（渲染侧连一次全屏 pass 都不注入，雾彻底消失），
	 *          都在视野源收集**之前**判定 —— 不画雾的帧连 HashGrid 都不遍历：
	 *           ① **全局游戏状态不允许**：FFogOfWarCanDrawFogProvider（本工程注册的是
	 *              UMassBattleGlobalVarFunctionLibrary::CanDrawFog —— 编辑场景 / 主菜单不画，
	 *              模拟·训练·调试画）；
	 *           ② **本插件总开关关闭 / Mass 子系统缺失 / 世界正在销毁**；
	 *           ③ **观察队伍是观察者（INDEX_NONE）或管理员（127）** —— 这两个角色要能不受遮蔽地
	 *              查看整张地图。注意"提供者未注册"也会得到 INDEX_NONE，因此同样落在这一支：
	 *              无从判断"当前是谁在看"时，不遮蔽是更保守的一侧（后果至多是看不到雾，
	 *              而不是把一个缺失的注册变成"把谁的视野糊掉"）。
	 *
	 *          三道闸门共用一个出口：除了告诉渲染侧"不遮蔽"，它**还必须撤掉状态发布**
	 *          （FMassBattleFogVisionField::bEnabled = false）—— 否则消费方（Agent 渲染处理器的
	 *          敌方网格遮蔽、音频门控）会继续按上一帧的快照遮住敌人。新增"不画雾"的分支时必须走
	 *          那个出口，不能各自 return。
	 *
	 *          与之相对，"雾该画、只是本帧一条视野源都没收到"（相机移出所有单位视野 / 源被视图
	 *          剔除干净）走的是正常路径：渲染侧照常注入 pass、覆盖率场全 0，结果是**整屏遮蔽**。
	 *          这些结果在渲染侧由 bSceneFogActive 与"源数为 0"两个独立条件区分。
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
	 *              TryGetVisionSourceRadius / TryGetVisionSector）：FMassVisionFragment::SightRadius 必须为正，
	 *              生效半径 = SightRadius + SceneGpuVisionSourceRadiusPadding，并按 FTeam::index 归队；
	 *              揭雾形状（圆 / 扇形）也一并给出（FMassVisionFragment::SightAngleDegrees 与
	 *              FRotating::Direction），因此 CPU 消费者不需要自己再判一次扇形。
	 *              与 GPU 侧只有两点不同：① 不按"当前观察队伍"过滤，而是每个队伍各一份；
	 *              ② 不受 bEnableSceneGpuVisionSources 开关影响（那是渲染开关，而探索累积属于
	 *              逻辑/观测需求）。每队的收集上限是 MaxCpuVisionSourcesPerTeam（默认 0 = 不限，
	 *              与渲染侧的 MaxSceneGpuVisionSources 解耦：见该属性注释里"两条上限性质不同"的说明）。
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

	/**
	 * @brief 视野场的**持久**纹理（PF_R8G8：R = 当前视野覆盖率，G = 已探索）—— 对外唯一的视野场出口。
	 *
	 * @details 为什么需要它：这张场必须**跨帧存在** —— G 通道（已探索）是累积出来的历史，
	 *          R 通道（当前覆盖率）也只是每帧被增量更新，都出不了那一帧的 RDG 瞬态资源。
	 *          所以打包趟**直接写进这张持久 RT**（RDG 用 RegisterExternalTexture 挂在它上面），
	 *          合成趟再把它当外部纹理读回来。
	 *
	 * @details 尺寸随视野场几何变化（换图 / 场分辨率变化）时重建；内容会被**清成 R = 255（全可见）**，
	 *          这是刻意的安全默认：万一在第一次打包之前就被读取，得到的是"什么都能看见"，
	 *          而不是"全部被遮蔽"。见 EnsureVisionFieldTexture。
	 *
	 * @note 这张纹理**只在 GPU 上**，游戏线程读不到（回读等于每帧同步）。因此"敌方该不该画"
	 *       不走它，而是走每帧发布的视野源集：见 FMassBattleVisionSourceSet 与
	 *       MassBattleAgentRenderProcessor 的 ShouldHideAgentByFog。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> VisionFieldTexture = nullptr;

	/// @brief VisionFieldTexture 当前对应的场分辨率（0 = 还没有纹理）。
	/// @details 自己记着尺寸，而不是每帧去问纹理 —— 重建判据必须能在"不该重建"这一侧做到零 API 调用：
	///          本函数每帧都会走一次，稳态下只应是一次整数比较。
	FIntPoint VisionFieldTexelCount = FIntPoint::ZeroValue;

	/// @brief 场景雾的渲染实现。Activate 时注册进引擎，成员释放即反注册。
	TSharedPtr<FFogOfWarSceneViewExtension, ESPMode::ThreadSafe> SceneFogViewExtension;

	/// @brief 本帧的视野源清单，元素为 (WorldX, WorldY, Radius, Reserved)。
	/// @details 每帧 Reset（保留容量）后 Add，Num 即本帧源数；交给渲染线程时整个数组被交换走，
	///          下一帧换回上一帧的缓冲继续复用，因此稳态下不产生分配。
	TArray<FVector4f> SceneGpuVisionSources;

	/// @brief 同一批视野源的 CPU 版本：圆 (CenterX, CenterY, 真实视距 cm) 与扇形 (ForwardX, ForwardY, cos 半角)。
	/// @details 与 SceneGpuVisionSources 同源同序，但用**未加余量**的视距、且收集于视图剔除之前；
	///          每帧随快照一起发布给 MassBattle 的视野源查询集（见 FMassBattleVisionSourceSet）。
	///          之所以用 `FVector4f` 而不是那边的结构体：**本头文件是公开头**，不应依赖 MassBattle
	///          的类型（它对本插件是私有依赖）—— 发布时在 .cpp 里按两条数组转交。
	TArray<FVector4f> CpuVisionSourceCircles;
	TArray<FVector4f> CpuVisionSourceDirs;

	/// @brief 与 SceneGpuVisionSources **一一对应**的扇形参数：(dirX, dirY, cosHalfAngle, 0)。
	/// @details cosHalfAngle >= 1 表示"全向"（着色器据此跳过扇形判定）；否则像素相对源中心的方向与
	///          (dirX, dirY) 的夹角必须 <= halfAngle 才算被这条源揭雾。
	///          朝向取自单位自身的 FRotating::Direction（每帧变，因此不在 FMassVisionFragment 里缓存），
	///          张角取自 FMassVisionFragment::SightAngleDegrees（由 Bootstrap 从索敌 Common 参数写入）。
	/// @note 两条数组必须**同增同删**：收集循环把它们当作一个整体（见 .cpp 里的
	///       FFogSceneVisionSourceEntry），沿用同一套视图剔除与圆盘包含剔除判定。
	TArray<FVector4f> SceneGpuVisionSourceDirs;

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
	 * @param bSceneFogActive 本帧是否应该用雾遮蔽画面。**它不等于"有没有视野源"**：
	 *          - false（雾开关关闭 / 子系统不可用）→ 渲染侧不注入 pass，画面完全不受遮蔽；
	 *          - true 且本帧一条源都没有（相机移出所有单位的视野范围、或源被视图剔除干净）
	 *            → 渲染侧照常注入 pass，覆盖率场全 0，整屏按"从未探索"遮蔽 —— 这才是"什么
	 *            都看不见"应有的画面，而不是让迷雾凭空消失。
	 */
	/**
	 * @brief 确保 VisionFieldTexture 与给定场分辨率一致（不一致才重建，内容清成 R = 255 全可见）。
	 *
	 * @details 重建是**换图级别**的事件（场分辨率由地图矩形与 VisionFieldTexelSizeCm 派生，稳态不变），
	 *          所以这里可以承受一次 FlushRenderingCommands —— 它保证渲染线程不再持有旧纹理，
	 *          否则释放旧 RHI 资源时可能正被上一帧的 RDG 用着。
	 *
	 * @details 为什么清成"全可见"而不是全黑：内容为 0 的语义是"哪里都不可见"，一旦消费方在打包趟
	 *          写出第一帧之前就开始采样（例如首帧的渲染顺序差），全黑会让**整场敌人与特效消失**。
	 *          全可见则是"遮蔽功能暂时不生效"，是同一个错误里唯一无害的那一侧。
	 */
	void EnsureVisionFieldTexture(FIntPoint InTexelCount);

	void UploadSceneGpuVisionSources(bool bSceneFogActive);

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
