# GPU 场景战争迷雾（世界空间视野场 + 记录层）

场景战争迷雾由**三趟 RDG pass** 实现，不使用后处理材质，也不用屏幕空间的几何散射。

视野形状由每个单位自己的索敌配置决定：**张角 360° 是圆盘，小于 360° 是以单位朝向（`FRotating::Direction`）为中轴的扇形**。
关键在于 GPU（画面）与 CPU（已探索层 / 当前可见层）用的是**同一份形状解析与同一个裁剪判据** ——
形状的解析只有一处实现（`FogOfWar.cpp` 的 `TryGetVisionSector`）：
两条收集链都从它取“中轴 + cos(张角 / 2)”，因此不存在“屏幕上被雾遮住、小地图却标成已探索”这类两套视野互相打架的情况。

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| `AFogOfWar` | `Source/FogOfWar/Private/FogOfWar.cpp` | 收集视野源（HashGrid + 队伍过滤 + 两道剔除），解析揭雾形状，算场几何，取已探索层，把三者交给渲染线程 |
| `FFogOfWarSceneViewExtension` | `Source/FogOfWar/Private/SceneFog/` | 世界作用域的 SceneViewExtension，在 Tonemap 之后插入三趟 pass |
| `FogOfWarScene.usf` | `Shaders/Private/FogOfWarScene.usf` | `VisionFieldSplatCS`（散射）+ `VisionFieldPackPS`（打包 R8G8）+ `CompositeVS/PS`（合成） |
| `FFogOfWarExploredLayerProvider` | `Source/FogOfWar/Public/` | “历史已探索层”的注册点；本工程里由 `UMassBattleMapSubsystem` 注册 |
| `UMassBattleMapSubsystem::RasterizeVisionShapes` | `Plugins/MassBattleAgent/Source/MassBattleMap/Private/MassBattleMapSubsystem.cpp` | CPU 侧的同一套视野形状：逐格落盘“已探索层 + 当前可见层” |

## 为什么不是后处理材质

材质图没有 scatter 能力：一个像素不能写多个像素，也不能读结构化缓冲做原子追加。因此“每个屏幕像素遍历全部视野源”是材质路线唯一能表达的形式，成本是 `O(屏幕像素数 × 源数)` —— 1080p × 500 源就是约 1e9 次内层迭代。

旧材质路线（`M_FogOfWarPostProcessing`、`FOW_*` 材质参数、`FOW_SceneGpuVisionSourceTexture`）已整体删除，参数只有 C++ 属性这一个真值源。

## 为什么覆盖率算在世界空间

早期实现是屏幕空间的几何散射（每条源一个实例化多边形 + `BO_Max` 混合）。它把复杂度降到了 `O(Σ 圆盘屏幕面积 + 屏幕像素数)`，但重叠区会被**反复着色**：`max` 混合要求每个被覆盖的像素都完整跑一遍像素着色器，源越密、相机越近，这份重复就越贵。而几何路线无法摆脱它 —— 逐像素只保留“最优源”需要深度测试，但最优判据 `半径 - 距离` 是逐像素量，无法在光栅化前作为顶点级深度给出。

改成 compute 之后，重叠区只剩**整数原子比较**：

```hlsl
InterlockedMax(VisionCoverage[texel], (uint)(Coverage * 255.0 + 0.5));
```

没有多边形、没有混合单元、没有逐层像素着色 —— 这就是“重叠 overdraw 被彻底消掉”的全部含义。

顺带解决的两件事：

1. **AABB 变成纯算术**：世界圆 → 场纹素矩形只有除法与 clamp，不含任何相机投影。屏幕空间方案需要在 CPU 端算圆的屏幕 AABB（透视下圆的投影是椭圆，还要处理“源落在相机后方 / 掠射角退化”），是一整类必须在 CPU 端特判的边界情形。
2. **与相机解耦**：每条源的 AABB 与相机无关，多视口的 AABB 完全一致，且场与“逐格已探索层”同世界矩形、同 UV。

## 三趟 pass

### 趟 1 `FogOfWar.VisionFieldSplat(N)`

一个线程组负责一条视野源，组内 8×8 的线程按步长扫过这条源的 AABB：

```cpp
FIntVector(1, 1, SourceCount)   // 组数：Z 维就是源数
```

因此派发出来的线程**全部**落在“这条源真正可能覆盖的纹素”上，不存在“全屏 × 全源”的乘积项。每条纹素的覆盖率：

```hlsl
Coverage = 1 - smoothstep(Radius - EdgeWidth, Radius, Distance)   // EdgeWidth == 0 时走硬边分支
```

写进 `PF_R32_UINT` 场（`InterlockedMax` 只能在整数格式上做）。命中 AABB 之外或覆盖率 ≤ 0 的纹素直接跳过，不产生任何写入。

**扇形裁剪**发生在同一趟、同一条纹素的半径判定之前：

```hlsl
// Dir.z 是 cos(半角)。>= 1 表示全向（CPU 对张角 >= 360 的源写 2.0），整段跳过 —— 圆形揭雾的老行为一字不改。
const float4 Dir = VisionSourceDirs[SourceIndex];
if (Dir.z < 1.0 && Distance > 1e-4)
{
    if (dot(ToTexel / Distance, Dir.xy) < Dir.z) continue;   // 落在扇形外：完全不揭雾（硬边）
}
```

几个容易踩的点：

- **判据是“纹素相对源中心的方向”**，与 CPU 侧逐格内核用的是同一个式子（两边各自做一次 `dot >= cosHalfAngle`，不共享数学但共享定义）。
- **`Distance > 1e-4` 的守卫不是可选项**：`ToTexel` 在源所在纹素上近似为零向量，归一化会出 NaN，而 NaN 参与比较恒为 false —— 后果是**源自己脚下那个纹素反而不揭雾**。CPU 侧对应的是“距离平方 ≈ 0 时直接算命中”。
- **扇形边界是硬边**：`SceneFogEdgeWidth` 只软化半径方向。两条直边做软化需要在角度域再插一档 `smoothstep`，而扇形在 Mass 单位上本就是索敌范围的近似表达，多这一档只是把边界挪几度；不软化反而让“画面上的雾边界”与“索敌的判定边界”完全重合，排查问题时少一个变量。
- **AABB 仍是圆的外接矩形**：扇形裁剪只在逐纹素做，这一趟的 AABB 不随张角收紧（GPU 侧每次只扫一条源，多出来的纹素被 `continue` 掉即可）。CPU 侧不一样 —— 那里每次刷新的遍历量与“每条源的框”成正比、且要覆盖全图，所以它在预处理阶段就按扇形外接框把框收紧了，见下文。

### 趟 2 `FogOfWar.VisionFieldPack`

全屏像素着色器（顶点由 `FPixelShaderUtils` 提供），把整数覆盖率与已探索层打包成一张 `PF_R8G8`：

```hlsl
OutColor = float2(Coverage, Explored);   // R = 当前视野覆盖率，G = 已探索
```

`Coverage` 用 `Load` 读（整数场与输出纹素一一对应，点采样就是它唯一正确的读法）；`Explored` 自己做了双线性（数据是位流而非纹理，采样器用不上），把逐格的 0/1 边界软化到场纹素尺度。

**两通道放进同一张纹理**，是为了让合成趟一次采样就能同时拿到“现在能不能看见”与“以前有没有看见过”—— 这正是灰雾需要的两个量。

### 趟 3 `FogOfWar.Composite`

一个覆盖整个视口的三角形。顶点阶段把三个角反投影到 `Z = SceneFogWorldPlaneZ` 平面，透视校正插值后每个像素就拿到自己的世界 XY，再换算成场 UV（像素阶段不必再做一次反投影）。

```hlsl
Brightness = NotVisibleRegionBrightness;                       // 从未探索
Brightness = lerp(Brightness, ExploredRegionBrightness, G);    // 探索过但当前不可见 → 灰雾
Brightness = lerp(Brightness, 1.0, R);                         // 当前可见 → 原色
OutColor = SceneColor * Brightness;
```

位置是 `EPostProcessingPass::Tonemap`，等价于旧材质的 `After Tonemapping`。场外的世界位置（相机看向地图之外）按“从未探索”处理 —— 不能只靠 clamp 采样兜底，那会把地图边缘那一列纹素一路拉伸到屏幕外。

### 反投影的写法

```hlsl
float4 NearWorld4 = mul(float4(NDC, 1.0, 1.0), InvViewProjection);
float4 FarWorld4  = mul(float4(NDC, 0.5, 1.0), InvViewProjection);
```

用两个不同的裁剪 z 反投影出两个**有限远**的世界点再作差，而不是“相机位置 + 近平面点”：

1. 不依赖远平面有限远 —— UE 的透视投影远平面在无穷远，`z = 0` 处的反投影 `w` 分量为 0，除法会炸；
2. 不区分透视/正交 —— 正交相机的 `ViewLocation` 并不是像素射线的起点，用两点差天然绕开这个坑。

## 两个必须遵守的实现约束（按引擎源码核对过）

1. **每个入口点引用到的全局变量，必须出现在它自己那个着色器类的 `FParameters` 里。** UE 的着色器编译器是按“该入口点用到的名字”去根参数结构里查的，查不到直接编译失败：`Shader parameter X could not be bound to <Shader>'s shader parameter structure`（`ShaderCompilerCommon.cpp`）。所以 `SceneFogEdgeWidth` 属于 `VisionFieldSplatCS::FParameters`（散射趟自己用），`FieldTexelCount` 属于 `VisionFieldPackPS::FParameters`（打包趟按纹素算 UV），`SceneColorUVScaleBias / InvViewProjection / FogPlaneZ / FieldWorld*` 属于合成 VS，纹理/采样器/亮度属于合成 PS。**两个阶段各自调一次 `SetShaderParameters`，不共用一份结构**；结构里多出本阶段用不到的成员无害，少一个就是编译错误或取值未定义。
2. **`Inputs.OverrideOutput` 无效时必须另建一张输出纹理，不能就地写场景色。** `OverrideOutput` 只在“本回调是该 pass 之后最后一个写这张纹理的环节”时有效（引擎的 `AcceptOverrideIfLastPass`）；Tonemap 之后还挂着 FXAA / SMAA 时它就是无效的 —— 而那是默认配置，所以这条不是边角分支。就地写会让同一张纹理在同一趟里既当 SRV 又当 RTV（合成阶段还要按 UV 采样它），属于未定义行为；引擎的 `PostProcessMaterial` 出于同样理由也另建一张中间纹理。

另有一条 `FPixelShaderUtils::AddFullscreenPass` 专属的约束：它要求**像素着色器的 `FParameters` 自己带上 `RENDER_TARGET_BINDING_SLOTS()`**（它不接受另拆一份 pass 参数）。打包趟只有一个阶段，所以直接照办；合成趟用手写的 `GraphBuilder.AddPass`，才需要把 pass 参数与阶段参数分开。

## AFogOfWar 参数

```text
bAutoActivate                              是否 BeginPlay 自动激活
bEnableSceneGpuVisionSources               总开关；关闭时渲染侧连 pass 都不注入（画面无遮蔽，而非全遮蔽）
MaxSceneGpuVisionSources                   每帧最多交给 GPU 的视野源数（超出按遍历顺序截断）
SceneGpuVisionSourceRadiusPadding          每条源的额外半径余量（同时覆盖投影平面高度差，不建议设 0）
FogEdgeWidth                               视野圆边缘软化宽度；0 = 硬边
SceneFogWorldPlaneZ                        视野圆（以及整个视野场）所在的世界 Z 平面（默认 0 = 地面基准面）
VisionFieldTexelSizeCm                     期望的场纹素世界边长（cm）；场分辨率 ≈ 地图尺寸 / 这个值
NotVisibleRegionBrightness                 从未探索区域的亮度
ExploredRegionBrightness                   已探索但当前不可见区域的亮度（灰雾）；低于上者时自动抬到上者
bCullVisionSourcesOutOfView                是否剔除完全落在视口外的源
VisionSourceCullExtraMarginCm              视图剔除的额外余量
bEnableSceneGpuVisionPerformanceStats      是否统计耗时/数量
SceneGpuVisionPerformanceLogInterval       统计周期（秒）
```

这些参数每帧随视野源一起作为一份快照交给渲染线程，因此**在 PIE 里改动能立刻生效**。

## 什么时候完全不画雾

`bSceneFogActive = false` 的帧，渲染侧连一次全屏 pass 都不注入 —— 画面就是场景本身，没有雾。
它与"雾开着但本帧一条视野源都没收到"（覆盖率场全 0 → **整屏遮蔽**）是两种不同语义，别混。
产生前者有三道闸门，都在视野源收集**之前**判定（不画雾的帧连 HashGrid 都不遍历，成本为零）：

| # | 闸门 | 触发条件 | 语义 |
| --- | --- | --- | --- |
| 1 | `FFogOfWarCanDrawFogProvider` | 注册方判定"现在不该画雾" | 业务侧按**全局游戏状态**给出的裁决 |
| 2 | `bEnableSceneGpuVisionSources` | 开关关闭 / Mass 子系统缺失 / 世界正在销毁 | 本插件自己的总开关（"这次不用它"） |
| 3 | `FFogOfWarViewingTeamProvider` | 观察者是 `INDEX_NONE`（观察者）或 `127`（管理员） | 看的人要看整张地图 |

闸门 1 的实现是本工程的 `UMassBattleGlobalVarFunctionLibrary::CanDrawFog()` ——
按 `EGlobalGameState` 判定：**编辑场景与主菜单不画，模拟 / 训练 / 调试画**。
由于依赖方向（`MassBattleSystem → MassBattleMap → FogOfWar`）不允许本插件反向调用它，
它和闸门 2 一样经提供者注册进来（注册点：`FMassBattleSystemModule::StartupModule`）。

三道闸门都汇入**同一个出口** `SkipFogThisFrame()`，它做两件事，缺一不可：

1. 告诉渲染侧"不遮蔽"（`bSceneFogActive = false` → 连一次全屏 pass 都不注入）；
2. **撤掉视野场的发布**（`FMassBattleFogVisionField::bEnabled = false`）。

第 2 件容易被忽略：消费方（Agent/FX 渲染器的可见性裁剪、音频门控）手里握着的是**上一帧的快照**，
只做第 1 件会留下"画面上没有雾、敌人却依然被遮住或被静音"的半生效状态。这也正是
`FMassBattleFogVisionField::bEnabled` 存在的理由 —— 所以每一条"不画雾"的出口都必须经过它，
不能再各自散落地 `return`。

两处刻意的退化取向（都是"缺信息 → 不遮蔽"）：

- 观察队伍提供者未注册时返回 `INDEX_NONE`，于是走闸门 3、不画雾。本插件无从判断"当前是谁在看"，
  不遮蔽是更保守的一侧 —— 后果至多是"看不到雾"，而不是"一个缺失的注册把谁的视野糊掉"。
- `CanDrawFog` 提供者未注册时按**允许**处理（插件独立使用时的默认行为就是画雾）；
  且 `CanDrawFog()` 自身在全局变量子系统不可用时返回 false，此时同样不画。

两种情况各打一条一次性日志（`LogFogOfWar`），把成因写出来，免得日后把"没注册"误当成"某个模式生效了"。

@note 这道闸门只关**表现**（雾、敌人遮蔽、敌方声音），**不关数据**：CPU 侧的逐队探索层
（`CollectVisionSourcesByTeam` → 已探索/可见层 → RL 观测的 `explored_percent` / `bVisible`）
照常累积。它不是"画出来的东西"，而是逻辑与观测的输入，编辑场景里也没有它的消费者。
（同理，观察者/管理员模式也不影响探索层 —— 见 `RefreshExploredFromFogOfWar` 的说明。）


**揭雾形状不在这张表里** —— 它是逐单位的数据，不是全局开关，来源只有一条链：

```text
FTrace::Mode == SectorTraceByTraits
  → FTrace::SectorTrace::Common::TraceRadius / TraceAngle      （半径与张角取自同一组参数，不会错配）
  → UMassBattleFogOfWarBootstrapProcessor 写入 FMassVisionFragment::SightRadius / SightAngleDegrees
  → 收集时现读 FRotating::Direction 作为中轴                     （朝向每帧变，不缓存）
  → cos(张角 / 2) ≥ 1 ? 全向 : 扇形
```

手工配置的单位（`UMassVisionTrait`）直接写 `FMassVisionFragment::SightAngleDegrees` 即可，默认 360 = 全向。
想看某个单位的实际形状，读 `CollectVisionSourcesByTeam()` 的结果里的 `HalfAngleCos`：`>= 1` 就是全向，
否则 `acos(HalfAngleCos) * 2` 是它的张角（度）。

## CPU 侧收集

每帧 Tick 走一趟（唯一实现：`FogOfWar.cpp` 内的 `TryGetVisionSourceRadius` / `TryGetVisionSector` / `IsDiscContainedIn`）：

0. 先问“当前观察队伍是谁”（`FFogOfWarViewingTeamProvider`）：若是**观察者（`INDEX_NONE`）或管理员（127）**，
   本帧直接结束 —— 不遮蔽画面、也不遍历 HashGrid（见上一节）；
1. 遍历 `UMassBattleHashGridSubsystem::AgentGrid` 的每个 block / cell / agent；
2. 只要带 `FMassVisionFragment` 且 `SightRadius > 0` 的 Agent，生效半径 = `SightRadius + SceneGpuVisionSourceRadiusPadding`；
3. 按观察队伍过滤（走到这一步它必定是一个具体队伍），只留同队单位的视野源；
4. **揭雾形状**：张角来自 `FMassVisionFragment::SightAngleDegrees`（Bootstrap 从索敌 `Common::TraceAngle` 写入），朝向**当场**读 `FRotating::Direction`（朝向每帧都变，缓存进碎片等于留一份注定过期的影子真值）。两者一起压成“中轴 + cos(张角/2)”，`cos >= 1` 是全向哨兵；
5. **视图剔除**：完全落在当前视口之外的源不可能覆盖任何输出像素（集合等价，非近似）；
6. **圆盘包含剔除**：源 A 的圆盘完全落在源 B 的圆盘内（`dist + rA <= rB`）时删掉 A 严格无影响（场取覆盖率最大值）。搜索范围限于同一个 HashGrid 格，因此是“可靠的但未必穷尽”。⚠ 对扇形源这是**保守**的（圆 ⊇ 扇形，被删的源的扇形一定仍被保留下来的圆盖住），但方向必须跟着一起删 —— 所以 `CellDirs` 与 `CellSources` 同索引增删，漏一个就会让扇形参数与源错位；
7. 受 `MaxSceneGpuVisionSources` 截断后交给渲染线程。

同一批数据也按队伍暴露给 CPU 侧消费者：`CollectVisionSourcesByTeam()`（`FFogVisionSource`：中心 + 两条半径口径 + 中轴 + `HalfAngleCos`），供探索层等逻辑自行累积历史（本插件只维护“当前帧可见性”）。

## CPU 侧视野形状（探索层 / 当前可见层）

`UMassBattleMapSubsystem` 是 CPU 侧的同一条视野链路：它拿 `CollectVisionSourcesByTeam()` 的结果，逐格落盘两支队伍的**已探索层**与**当前可见层**（`RasterizeVisionShapes`，两层共用一次遍历）。形状处理与 GPU 侧一一对应：

| | GPU（画面） | CPU（探索层 / 可见层） |
| --- | --- | --- |
| 形状来源 | `FFogVisionSource` → `SceneGpuVisionSourceDirs` | `FFogVisionSource` → `FVisionSource::ForwardDir / HalfAngleCos` |
| 全向哨兵 | `cosHalfAngle >= 1` → 着色器整段跳过 | `HalfAngleCos >= 1` → `bSector = false`，逐格判定里整段跳过 |
| 裁剪判据 | `dot(ToTexel / Distance, Dir.xy) < Dir.z` → `continue` | `Dx * Fx + Dy * Fy >= CosHalfAngle * sqrt(DistSq)` → 两层都不写 |
| 源自己所在单元 | `Distance > 1e-4` 守卫（NaN 防护） | `DistSq <= 1e-6` 时直接算命中（避免脚下留洞） |
| 外接框 | 保持圆的外接矩形 | 用扇形外接 AABB 收紧（见下） |
| 边界 | 半径方向由 `SceneFogEdgeWidth` 软化，直边硬 | 全硬边（格心口径，格边长本身就是量化粒度） |

几处 CPU 侧特有的取舍：

- **扇形外接框**：CPU 每次刷新要扫全图 × 每条源，所以预处理阶段就按“角度区间上 cos 的极值”算出扇形 AABB 并夹进圆的安全框（`IntervalCosMax / IntervalCosMin`）。窄扇形（例如 60°）的 X 跨度可能只有圆的一半量级，不收就等于白扫大半的行。框是**安全超集**（两端各外扩一格吸收取整误差），精确性仍由逐格判定保证。
- **方向缺失时退化为全向，而不是退化为 +X**：拿不到 `FRotating`、或方向是零向量时按全向处理（这里是 `TryGetVisionSector` 返回 false，不是“填个默认方向继续当扇形”）。最坏是“雾少遮了一块”，而按 +X 处理会让单位的视野整块**指错方向** —— 前者是保守的视觉误差，后者是把可见区域判错，代价不对等。CPU 侧还额外有一道“方向向量近似单位长度”（`DirLenSq > 0.5`）的门槛作为纵深防御：一条被写坏的记录不会变成“几乎什么都看不见”（那会直接把该队可见层抹空，而 RL 观测的 `bVisible` 就建在它上面）。
- **形状必须参与“输入未变”的比较**（`AreVisionSourcesEqual`）：扇形单位**原地转身**时位置与半径都没变，但可见层内容整片改变。漏比这两项 = “转向后可见性永久停在转身前那一帧”，且完全无声。
- **两层共用同一形状**：形状描述的是“这个单位往哪看、看多宽”，是视野源自身的属性，与“用含余量的半径还是真实视距”正交。所以它不做两套 —— 扇形裁剪对两条半径都只是“先按角筛、再按半径筛”。

## 历史已探索层（记录/灰雾通道）

本插件不持有历史。历史的权威在 `UMassBattleMapSubsystem` 的逐队探索层里，它通过 `FFogOfWarExploredLayerProvider` 把只读视图借出来：

```cpp
FFogOfWarExploredLayerProvider::Set(FFogOfWarGetExploredLayerDelegate::CreateUObject(
    this, &UMassBattleMapSubsystem::GetExploredLayerView));
```

为什么要反过来由业务侧注册：依赖方向是 `MassBattleSystem → MassBattleMap → FogOfWar`，FogOfWar 读 MassBattleMap 会成环。改为“插件声明契约、业务侧实现”，未注册时渲染侧退化成“可见 / 不可见”二态，不报错也不停摆 —— “没有历史”是一个合法状态，不是失败。

几处设计取舍：

- **位打包而非逐格字节**：200×200 的位图是 5KB，展开成字节是 40KB、按 uint 是 160KB，而这几 KB 每帧都要上传。位序与外部位图完全一致（LSB-first），因此从位图拷进来只是一次 `memcpy`，CPU 端零展开。
- **版本号**：`AFogOfWar` 只在提供者给的版本号变化时重新取，渲染侧也只在快照版本号变化时重新拷贝到渲染线程（多视口同帧的第二次 Subscribe 同样会跳过）。
- **场矩形由提供者决定**：视野场必须与逐格探索层同世界矩形，否则灰雾会在画面上**整体错位**（不只是边缘不齐）。拿不到提供者时才退回 `WorldGridSize` 派生的范围。
- **拿不到就清空**：提供者不可用时主动清掉灰雾回到二态，而不是继续展示上一次的快照 —— 那份数据可能属于另一张地图或另一支队伍。
- **`AFogOfWar::Activate()` 会作废版本号**：Actor 在同一进程里被复用（换图重进 / PIE 重开）时，新扩展没有任何快照，若沿用旧版本号会永远收不到已探索层。

## 线程模型

- **游戏线程**：`UpdateSceneGpuVisionSources()` 收集清单 → 刷新已探索层 → `UploadFrameData_GameThread()` 加锁交换清单与参数快照、`UploadExploredLayer_GameThread()` 单独交已探索层（它的数据量比视野源大两三个数量级，不值得每帧搬）。游戏线程不碰任何图形资源。
- **渲染线程**：`SubscribeToPostProcessingPass()` 取一份快照（多视口时同一帧会多次 Subscribe，因此是拷贝而不是交换），只有“本帧不该遮蔽”（雾总开关关闭 / 子系统不可用 / **观察队伍是观察者或管理员**，见上一节）或场几何未就绪时才不注入回调；回调里按 RDG 建缓冲与纹理并记录三趟 pass —— 本帧没有视野源时跳过上传与散射趟、只保留清零后的覆盖率场，于是整屏被判成“从未探索”而被遮蔽。RDG 的 `QueueBufferUpload` 会自己拷贝源数据，因此不存在跨帧生命周期问题。

## 已知取舍

- **投影平面**：视野形状（圆盘 / 扇形）固定投影在 `SceneFogWorldPlaneZ` 平面上。像素所在表面高出它 `h` 时，屏幕上会有约 `h / tan(俯仰角)` 的偏移（例如 55° 俯角、200 cm 高度 → 约 140 cm）。这个量级由 `SceneGpuVisionSourceRadiusPadding`（默认 300 cm）覆盖，因此“该看见的仍然看得见”；地形整体抬高时把 `SceneFogWorldPlaneZ` 设成地面高度即可消除偏移。合成趟的反投影用的是同一个平面，因此两边的误差同号，不会互相放大。⚠ 注意这个余量只补**半径**，扇形张角不受它影响，所以扇形的两条直边在斜视时会看到与地面网格一致的投影偏移。
- **场分辨率**：场分辨率 ≈ 地图尺寸 / `VisionFieldTexelSizeCm`，每轴上限 2048 纹素（超过时按比例放大纹素边长，场仍铺满整个地图矩形）。它不再跟随视口分辨率：相机推近时雾边不会变得更精确，而是由合成趟的双线性采样软化。这是拿“与相机解耦 + 重叠成本固定”换来的。
- **场纹理目前仍是每帧每视图重建**：场是世界空间的，理论上可以跨视图共享，但当前实现没有做跨帧/跨视图缓存（场是 transient 的）。多视口（分屏 / SceneCapture）会把散射做多遍。若将来需要，可把场提成持久资源并只在视野源变化时重算。
- **灰雾有延迟**：已探索层由 CPU 侧按自己的节奏刷新（本工程里是 0.1s 节流），因此“刚照亮过的区域立刻转灰”会比画面本身晚一个刷新周期。它不该被当成渲染问题去查。
- **扇形边界是硬边，且落在格/纹素中心上**：GPU 侧只软化半径方向（`SceneFogEdgeWidth`），CPU 侧一概硬边。逐格/逐纹素判定意味着边界有“一个单元宽”的量化误差（CPU 格边长 256 cm，比 GPU 纹素粗得多），所以**探索层的扇形边界会比画面的雾边界粗一档**。这是格网分辨率决定的，不是两套判据分叉 —— 判据本身逐字一致。
- **扇形让“输入未变则整段跳过”更难命中**：`AreVisionSourcesEqual` 把朝向纳入比较后，**转向中的单位**每次刷新都算作“输入变了”，于是那一队的整段跳过失效、探索层光栅照常跑。圆形的旧行为里只有“位置 / 半径变化”才会触发，现在朝向变化同样触发。这是正确性要求的代价（不比较朝向就会让可见层停在转身前那一帧），不是可以靠调参绕开的开销 —— 真要省，应该从“朝向变化幅度小于一个格尺度的量化阈值”入手，而不是把它从比较里删掉。
- **扇形依赖 `FRotating`**：朝向缺失（实体没有该 fragment）或方向为零向量时退化为全向。也就是说**定点炮台这类不写朝向的实体拿不到扇形**，只能拿到整圈视野；需要它按朝向揭雾时，得让它有一个有效的 `FRotating::Direction`。
- **作用范围**：注册的是世界作用域的扩展，因此该世界的**所有视图**都会走这三趟 pass（包括编辑器视口与 SceneCapture）。旧的 `UPostProcessComponent` 已经删除，不再有 `BlendRadius/BlendWeight` 这种“局部生效”的语义。
- **平台**：`ShouldCompilePermutation` 限定 SM5。依赖三件事：`PF_R32_UINT` 的 UAV 与 `InterlockedMax`、`PF_R8G8` 渲染目标、compute dispatch 的 Z 维（源数上限受 `GRHIMaxDispatchThreadGroupsPerDimension.Z` 约束，`MaxSceneGpuVisionSources` 的默认值远低于它）。

## 迷雾遮蔽敌方实体（可见性裁剪）

RTS 视角下敌方单位即使站在迷雾里也会被画出来 —— 因为渲染与战争迷雾本来就是两套互不知情的数据。
这一节说明现在怎么把它们接起来：**画面上的雾由合成趟画（GPU 侧，见前文三趟 pass），
"敌方网格该不该画"由 CPU 侧的渲染批次决定**。

### 数据流

```text
AFogOfWar::Tick（每帧）
  ├─ 收集本队视野源 → 上传给场景雾扩展 → 三趟 pass 画雾（与"该不该画敌人"无关）
  └─ 同一批源也发布给 CPU：SetVisionSources(...)（构建只读查询集）＋ Set(bEnabled = 本帧是否真要画雾)
                                    ↓
MassBattleAgentRenderProcessor::Execute（每帧、逐实体）
  └─ ShouldHideAgentByFog(位置, 队伍)：敌方 且 !FogVisionSources->IsWorldVisible(位置) → IsHiddenArray[i]
                                    ↓
AMassBattleAgentRenderer::Tick（每帧、每批一次）
  └─ SetNiagaraArrayBool("IsHidden_Array", IsHiddenArray)
                                    ↓
MassBattle_AgentMeshPrediction.ush → OutMeshVisible = !InIsHidden

MassBattleFxRenderProcessor::Execute（每帧、**串行**段，在推送之前）
  └─ 对"当前占用"的槽位按位置查可见性 → IsHiddenArray_Attached[slot]
  └─ SetNiagaraArrayBool("IsHiddenArray_Attached", …)（HostMono 的 CPU 插值批次推的是同一个数组）
                                    ↓
MassBattle_FxPrediction.ush → OutVisible = !InIsHidden
```

关键点：

- **走的是本来就存在的通道**（`IsHidden_Array` → `InIsHidden` → `OutMeshVisible`，池化/休眠一直在用它），
  所以**零资产改动**：不需要给 Niagara 加引脚、不需要给 `.ush` 加参数、不需要动任何资产。
- **只改可见性**：渲染批次数组与 sim 状态本来就是解耦的，被遮住的实体照常被处理器更新、照常参与
  移动/物理/命中/AI —— "迷雾只遮画面、不遮行为"是这套架构自带的，不是需要额外保证的东西。
- **队伍来自 CPU**：每实例的 `FTeam::index` 就在处理器手里，不需要经过打包进 `DynamicParams0.w`
  的那 8 位再解一次。

### 为什么不采样视野场纹理（曾经的方案）

曾经的做法是让 Niagara 在 shader 里采样视野场纹理（R8G8），好处是遮蔽边界与画面上的雾**逐像素重合**。
这条路在引擎层走不通，相关代码已全部删除：

- Niagara 的纹理是 **DataInterface**，不是 `Texture2D` 值 —— 不能当普通实参传给 `.ush` 的形参；
- 把它做成 Custom HLSL 节点的 DI 输入引脚也不行：`ProcessCustomHlsl` 要求 DI"已在编译数据里注册"
  （即经参数图 / DI 函数调用传进来），否则带着"签名作废"直接 `return`，调用方随即报
  `Incorrect number of outputs`，下游每个 `Set` 节点跟着报错；
- 想用 `User.FogVisionField` 这类命名空间读法同样不行：那段替换被参数图引脚门着，而那个模块的
  34 个引脚**全是普通值类型**，没有参数图引脚 → token 被替换成空串（`'User' undeclared`）。

结论：**那个模块的新输入只能是普通值类型**。要恢复"逐像素对齐"必须把判定拆开（资产里用 DI 的
函数形态采样、把采样结果当普通值传进来），代价是判据一半在资产、一半在 `.ush`。

### ⚠ 为什么不用 CPU 可见层位图（踩过的坑）

最初的实现用 `IsWorldVisibleForTeam`（地图子系统的"当前可见层"位图），结果**完全不生效**：
那张位图全工程**只有一个驱动点** —— RL 观测（`MassBattleSystemSubsystem::SpawnObservation`，
每个 RL 步一次、每次光栅化约 60ms，见那里的注释"全工程唯一驱动这两层的调用点"）。
PIE 里手动玩 / 不跑 RL 循环时它从未被光栅化，`HasVisibleLayer()` 为假、查询一律返回"可见"，
于是遮蔽一次都没发生；而画面上的雾是 GPU 每帧算的 —— 症状就是"雾在、敌人却还站着"。

现在改用**每帧都在收集**的那份视野源清单（GPU 揭雾的同一批源、同一份形状规则）做点查询：

- 半径用**未加余量的真实视距**（`SightRadiusCm`）：画面上雾的覆盖率含 300cm 余量，因此存在一条
  "雾已画出来、敌人已消失"的灰带 —— 方向安全（多遮而不是漏遮）。
- 查询集每帧重建一次（按 X 排序 + 半径窗口，6000 条源约 0.2ms），构建完只读，可被并行循环无锁共享。
- 与位图那条路相比，差别是**没有刷新周期滞后**，也不依赖 RL 是否在跑。

失效方向仍然安全：源集未发布 / 迷雾本帧未生效时一律按"不遮蔽"处理 —— 最坏是"多画了"，
而不是把整场敌人抹掉。

### 性能

| 项 | 成本 |
| --- | --- |
| CPU（每帧） | 单位：逐实体 1 次 O(1) 位查询 + 1 次比较，**只对敌方做**（同队直接跳过，不打任何查询）；特效：每个**占用**槽位 1 次位查询（串行段，在推送之前） |
| GPU | **零额外成本**：只是某个实例的 `IsHiddenArray` 由 false 变 true，Niagara 照常跑，`OutMeshVisible` 为假时不画 |
| 额外带宽 | 无：`IsHidden_Array` 本来每帧就在推 |
| 音频（每帧） | 每条在播声音 1 次 O(1) 位测试 + 1 次 `SetVolumeMultiplier`（几十条量级） |

万级单位下每帧多的是"1 万次位测试"，相对批次装配、动画解码、混音都可以忽略。
这条路刻意避开的正是最贵的那件事：逐帧在 CPU 上重算可见层（60ms/次）—— 我们只**查**它，不重算它。

### 已知边界

- **无刷新滞后**：网格遮蔽与音频静音都与画面上的雾用**同一帧**的视野源，逐帧一致，
  且都不再依赖 RL 是否在跑。那条"查地图可见层位图"的注册与查询已随本次改造删除
  （`FMassBattleTeamVisibilityProvider` 现在只提供"当前观察队伍"）。
- **没有 300cm 提前量**：遮蔽边界跟真实视距走，而画面上雾的边界含 300cm 余量 —— 于是存在一条
  "雾已经画出来、敌人也已经被遮住"的灰带。方向上安全（多遮而不是漏遮）。
- **只裁敌方，不裁己方**：在 C++ 里比较 `FTeam::index` 与观察队伍，同队直接跳过判定 ——
  所以"我的部队会不会消失"不依赖任何视野数据是否正确。
- **FX 只能按位置裁（没有队伍）**：特效实体不带 `FTeam`，而 FX 的逐实体循环与推送段都是**并行**的，
  从工作线程回查地图子系统的可见层不安全 —— 所以 FX 的遮蔽放在 `MassBattleFxRenderProcessor` 的
  **串行**推送段，只按位置判。己方特效通常就在己方视野内不受影响，但**远端的己方特效理论上会被误遮**。
  要按队伍隔离，得把发起者队伍带进 FX 批次（`FFxVisualizing::ParentEntity` 已经指回发起者，
  但它只能在游戏线程上查）。
- **音频只覆盖"被插值的声音"**：门控挂在 `UpdateHostRenderInterp` 的逐帧声音循环里，因此只作用于
  **附着型**（`bAttached`）声音宿主；一次性的 3D 音效（例如远处爆炸）走另一条路，目前不受遮蔽。
  要覆盖它需要另加一条逐帧遍历（`FSoundConfig_Final` 上的 fragment query）。
- **多视口**：外部 RT 每帧只被**第一个**视口写一次（同一张 RDG 纹理不能无序写两次）。视野源本来就会
  按视口做视图剔除，因此分屏下发布的场对应"最先渲染的那个视口"。要消除这一点，把
  `bCullVisionSourcesOutOfView` 关掉即可 —— 场与相机无关，关掉之后各视口算出的场完全一致。

### 已移除的尝试（不要再走）

为实现"逐像素对齐的网格遮蔽"，曾经写过一整条 Niagara 链路（着色器采样视野场纹理）。它在引擎层
走不通，相关代码已全部删除，留此备查：

| 删掉的东西 | 为什么 |
| --- | --- |
| `MassBattle_AgentMeshPrediction.ush` / `MassBattle_FxPrediction.ush` 里的 `MassBattle_FogVisibility` 与"迷雾参数重载" | 没有任何资产能把纹理喂进去（DI 进不了 Custom HLSL 模块），永远是死代码 |
| `FMassBattleFogVisionFieldProvider` 的 `Texture / WorldToUV / ViewingTeam / IsUsable / GetNiagara*ParamName / ApplyToNiagaraComponent` | 只服务那条死链路 |
| `AMassBattleAgentRenderer::Tick` / `AMassBattleFxRenderer::Tick` 里的 `SetVariableTexture…` / `SetVariableVec4` / `SetVariableFloat` 推送 | 同上 |

`FMassBattleFogVisionField` 现在只剩一个字段 `bEnabled`（本帧迷雾是否生效），消费方是
`MassBattleHostSubsystem`（敌方声音静音）与 `MassBattleAgentRenderProcessor`（敌方网格遮蔽）——
两者必须同源，否则会出现"声音被遮了、敌人却还站着"这类半生效状态。

验证：站己方视角把相机移到己方视野之外，**敌方**单位与它打出的特效应当消失，**己方完全不受影响**；
切观察者/管理员，雾与遮蔽**同时**消失（`bEnabled=false` 那条路）；切回来两者同时恢复。

## 测量

- 抓帧看 `FogOfWar.VisionFieldSplat(N)`（N = 本帧源数）、`FogOfWar.VisionFieldPack`、`FogOfWar.Composite`。
- 散射趟的成本与 `Σ 每条源 AABB 的纹素数` 成正比，与视口分辨率、与相机距离都无关；几何路线的散射成本则随相机推近而暴涨。对比这两个数字可以直接看出收益。
- 想验证 overdraw 确实被消掉：把 `VisionFieldTexelSizeCm` 调小（场更细）时成本应线性上升，而**不会**像几何路线那样按重叠层数成倍上升。
- **验证 CPU 与 GPU 是同一个扇形**：给一个单位设 `Common::TraceAngle = 90`，让它原地转身（不给位移指令）——
  画面上的雾边界应当跟着转，同时 `IsWorldLocationVisibleForTeam` 在它背后应当翻成 `false`。
  两边**同步**才说明形状只有一份；如果画面转了而 `bVisible` 没变，去看 `AreVisionSourcesEqual`（朝向没进比较）。
  转一圈后统计 `Saved/Logs` 里的“迷雾刷新整段跳过”次数会明显下降 —— 这是扇形带来的预期代价，不是回归。
- **量扇形裁剪的收益**：把 `TraceAngle` 从 360 调到 90，同一次刷新里 CPU 侧的两层光栅耗时应下降（外接框收紧 + 逐格角判）；
  GPU 侧的散射趟成本基本不变（AABB 仍是圆的外接矩形，只少了原子写入），差异主要落在“重叠区写入次数”上。
- `Saved/Logs/FogOfWar_ScenePerf.csv` 每 `SceneGpuVisionPerformanceLogInterval` 秒一行：
  `WorldTime,Channel,Samples,AvgTotalMs,AvgCollectMs,AvgUploadMs,AvgSourceCount,AvgVisitedCells,AvgVisitedAgents,AvgCulledSources,AvgContainedSources`
  `AvgUploadMs` 现在只记“交接给渲染线程”的耗时（应该是接近 0 的小量）；GPU 侧成本不再由这些 CPU 数字反映，请看抓帧。
