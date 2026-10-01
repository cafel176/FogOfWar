# GPU 圆形场景战争迷雾（世界空间视野场 + 记录层）

场景战争迷雾由**三趟 RDG pass** 实现，不使用后处理材质，也不用屏幕空间的几何散射：

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| `AFogOfWar` | `Source/FogOfWar/Private/FogOfWar.cpp` | 收集视野源（HashGrid + 队伍过滤 + 两道剔除），算场几何，取已探索层，把三者交给渲染线程 |
| `FFogOfWarSceneViewExtension` | `Source/FogOfWar/Private/SceneFog/` | 世界作用域的 SceneViewExtension，在 Tonemap 之后插入三趟 pass |
| `FogOfWarScene.usf` | `Shaders/Private/FogOfWarScene.usf` | `VisionFieldSplatCS`（散射）+ `VisionFieldPackPS`（打包 R8G8）+ `CompositeVS/PS`（合成） |
| `FFogOfWarExploredLayerProvider` | `Source/FogOfWar/Public/` | “历史已探索层”的注册点；本工程里由 `UMassBattleMapSubsystem` 注册 |

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

## CPU 侧收集

每帧 Tick 走一趟（唯一实现：`FogOfWar.cpp` 内的 `TryGetVisionSourceRadius` / `IsDiscContainedIn`）：

1. 遍历 `UMassBattleHashGridSubsystem::AgentGrid` 的每个 block / cell / agent；
2. 只要带 `FMassVisionFragment` 且 `SightRadius > 0` 的 Agent，生效半径 = `SightRadius + SceneGpuVisionSourceRadiusPadding`；
3. 按“观察队伍提供者”（`FFogOfWarViewingTeamProvider`）过滤，提供者缺失时退化为全场并集；
4. **视图剔除**：完全落在当前视口之外的源不可能覆盖任何输出像素（集合等价，非近似）；
5. **圆盘包含剔除**：源 A 的圆盘完全落在源 B 的圆盘内（`dist + rA <= rB`）时删掉 A 严格无影响（场取覆盖率最大值）。搜索范围限于同一个 HashGrid 格，因此是“可靠的但未必穷尽”；
6. 受 `MaxSceneGpuVisionSources` 截断后交给渲染线程。

同一批数据也按队伍暴露给 CPU 侧消费者：`CollectVisionSourcesByTeam()`，供探索层等逻辑自行累积历史（本插件只维护“当前帧可见性”）。

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
- **渲染线程**：`SubscribeToPostProcessingPass()` 取一份快照（多视口时同一帧会多次 Subscribe，因此是拷贝而不是交换），只有“本帧不该遮蔽”（雾总开关关闭 / 子系统不可用）或场几何未就绪时才不注入回调；回调里按 RDG 建缓冲与纹理并记录三趟 pass —— 本帧没有视野源时跳过上传与散射趟、只保留清零后的覆盖率场，于是整屏被判成“从未探索”而被遮蔽。RDG 的 `QueueBufferUpload` 会自己拷贝源数据，因此不存在跨帧生命周期问题。

## 已知取舍

- **投影平面**：视野圆盘固定投影在 `SceneFogWorldPlaneZ` 平面上。像素所在表面高出它 `h` 时，屏幕上会有约 `h / tan(俯仰角)` 的偏移（例如 55° 俯角、200 cm 高度 → 约 140 cm）。这个量级由 `SceneGpuVisionSourceRadiusPadding`（默认 300 cm）覆盖，因此“该看见的仍然看得见”；地形整体抬高时把 `SceneFogWorldPlaneZ` 设成地面高度即可消除偏移。合成趟的反投影用的是同一个平面，因此两边的误差同号，不会互相放大。
- **场分辨率**：场分辨率 ≈ 地图尺寸 / `VisionFieldTexelSizeCm`，每轴上限 2048 纹素（超过时按比例放大纹素边长，场仍铺满整个地图矩形）。它不再跟随视口分辨率：相机推近时雾边不会变得更精确，而是由合成趟的双线性采样软化。这是拿“与相机解耦 + 重叠成本固定”换来的。
- **场纹理目前仍是每帧每视图重建**：场是世界空间的，理论上可以跨视图共享，但当前实现没有做跨帧/跨视图缓存（场是 transient 的）。多视口（分屏 / SceneCapture）会把散射做多遍。若将来需要，可把场提成持久资源并只在视野源变化时重算。
- **灰雾有延迟**：已探索层由 CPU 侧按自己的节奏刷新（本工程里是 0.1s 节流），因此“刚照亮过的区域立刻转灰”会比画面本身晚一个刷新周期。它不该被当成渲染问题去查。
- **作用范围**：注册的是世界作用域的扩展，因此该世界的**所有视图**都会走这三趟 pass（包括编辑器视口与 SceneCapture）。旧的 `UPostProcessComponent` 已经删除，不再有 `BlendRadius/BlendWeight` 这种“局部生效”的语义。
- **平台**：`ShouldCompilePermutation` 限定 SM5。依赖三件事：`PF_R32_UINT` 的 UAV 与 `InterlockedMax`、`PF_R8G8` 渲染目标、compute dispatch 的 Z 维（源数上限受 `GRHIMaxDispatchThreadGroupsPerDimension.Z` 约束，`MaxSceneGpuVisionSources` 的默认值远低于它）。

## 测量

- 抓帧看 `FogOfWar.VisionFieldSplat(N)`（N = 本帧源数）、`FogOfWar.VisionFieldPack`、`FogOfWar.Composite`。
- 散射趟的成本与 `Σ 每条源 AABB 的纹素数` 成正比，与视口分辨率、与相机距离都无关；几何路线的散射成本则随相机推近而暴涨。对比这两个数字可以直接看出收益。
- 想验证 overdraw 确实被消掉：把 `VisionFieldTexelSizeCm` 调小（场更细）时成本应线性上升，而**不会**像几何路线那样按重叠层数成倍上升。
- `Saved/Logs/FogOfWar_ScenePerf.csv` 每 `SceneGpuVisionPerformanceLogInterval` 秒一行：
  `WorldTime,Channel,Samples,AvgTotalMs,AvgCollectMs,AvgUploadMs,AvgSourceCount,AvgVisitedCells,AvgVisitedAgents,AvgCulledSources,AvgContainedSources`
  `AvgUploadMs` 现在只记“交接给渲染线程”的耗时（应该是接近 0 的小量）；GPU 侧成本不再由这些 CPU 数字反映，请看抓帧。
