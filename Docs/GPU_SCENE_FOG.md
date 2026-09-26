# GPU 圆形场景战争迷雾（散射遮罩 + 合成）

场景战争迷雾由**两趟 RDG pass** 实现，不再使用后处理材质：

| 组件 | 位置 | 职责 |
| --- | --- | --- |
| `AFogOfWar` | `Source/FogOfWar/Private/FogOfWar.cpp` | 收集视野源（HashGrid + 队伍过滤 + 两道剔除），把清单与参数交给渲染线程 |
| `FFogOfWarSceneViewExtension` | `Source/FogOfWar/Private/SceneFog/` | 世界作用域的 SceneViewExtension，在 Tonemap 之后插入两趟 pass |
| `FogOfWarScene.usf` | `Shaders/Private/FogOfWarScene.usf` | `VisionSplatVS/PS`（散射覆盖率）+ `CompositeVS/PS`（合成） |

## 为什么不是后处理材质

材质图没有 scatter 能力：一个像素不能写多个像素，也不能读结构化缓冲做原子追加。因此"每个屏幕像素遍历全部视野源"是材质路线唯一能表达的形式，成本是 `O(屏幕像素数 × 源数)` —— 1080p × 500 源就是约 1e9 次内层迭代。散射写法把成本降到 `O(Σ 每个圆盘覆盖的屏幕面积 + 屏幕像素数)`：与源数线性，且没有"屏幕像素 × 源数"这个乘积项。

旧材质路线（`M_FogOfWarPostProcessing`、`FOW_*` 材质参数、`FOW_SceneGpuVisionSourceTexture`）已整体删除，参数只有 C++ 属性这一个真值源。

## 两趟 pass

**趟 1 `FogOfWar.VisionSplat(N)`**：每条视野源一个实例，顶点由 `SV_VertexID` 程序化生成一个 16 边**外接**正多边形（半径放大 `1/cos(π/16)`；用内接多边形会把最外圈软化段切掉，边缘变硬切）。顶点投影用 `FSceneView::ViewMatrices.GetWorldToClip()`，因此不假设相机是正交、也不假设地图朝向。像素阶段按世界距离算覆盖率：

```hlsl
Coverage = 1 - smoothstep(Radius - EdgeWidth, Radius, Distance)   // EdgeWidth == 0 时走硬边分支
```

输出到 `PF_R8` 覆盖率遮罩，**混合模式是 max**（`BO_Max`）：重叠圆盘必须取并集，否则后画的圆会在先画的圆上切出一道缺口。

**趟 2 `FogOfWar.Composite`**：一个覆盖整个视口的三角形，采样遮罩（双线性）后合成：

```hlsl
OutColor = lerp(SceneColor * saturate(NotVisibleRegionBrightness), SceneColor, Coverage)
```

与旧材质完全相同的合成语义（含 alpha 直通）。位置是 `EPostProcessingPass::Tonemap`，等价于旧材质的 `After Tonemapping`。

## 两个必须遵守的实现约束（按引擎源码核对过）

1. **每个入口点引用到的全局变量，必须出现在它自己那个着色器类的 `FParameters` 里。** UE 的着色器编译器是按"该入口点用到的名字"去根参数结构里查的，查不到直接编译失败：`Shader parameter X could not be bound to <Shader>'s shader parameter structure`（`ShaderCompilerCommon.cpp`）。所以 `SceneFogEdgeWidth` 属于 `FFogOfWarSceneSplatPS::FParameters`（只有像素阶段读它），`VisionSources / WorldToClip / FogPlaneZ` 属于 `FFogOfWarSceneSplatVS::FParameters`；`SceneColorUVScaleBias / CoverageMaskUVScaleBias` 属于合成 VS，纹理/采样器/亮度属于合成 PS。**两个阶段各自调一次 `SetShaderParameters`，不共用一份结构**；结构里多出本阶段用不到的成员无害，少一个就是编译错误或取值未定义。
2. **`Inputs.OverrideOutput` 无效时必须另建一张输出纹理，不能就地写场景色。** `OverrideOutput` 只在"本回调是该 pass 之后最后一个写这张纹理的环节"时有效（引擎的 `AcceptOverrideIfLastPass`）；Tonemap 之后还挂着 FXAA / SMAA 时它就是无效的 —— 而那是默认配置，所以这条不是边角分支。就地写会让同一张纹理在同一趟里既当 SRV 又当 RTV（合成阶段还要按 UV 采样它），属于未定义行为；引擎的 `PostProcessMaterial` 出于同样理由也另建一张中间纹理。

## AFogOfWar 参数

```text
bAutoActivate                              是否 BeginPlay 自动激活
bEnableSceneGpuVisionSources               总开关；关闭时交出空清单，渲染侧连 pass 都不注入
MaxSceneGpuVisionSources                   每帧最多交给 GPU 的视野源数（超出按遍历顺序截断）
SceneGpuVisionSourceRadiusPadding          每条源的额外半径余量（同时覆盖投影平面高度差，不建议设 0）
FogEdgeWidth                               视野圆边缘软化宽度；0 = 硬边
SceneFogWorldPlaneZ                        视野圆所在的世界 Z 平面（默认 0 = 地面基准面）
SceneGpuVisionMaskResolutionDivisor         遮罩分辨率分母：1 = 与视口同分辨率，2 = 半分辨率
NotVisibleRegionBrightness                 完全被雾覆盖区域的亮度
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
3. 按"观察队伍提供者"（`FFogOfWarViewingTeamProvider`）过滤，提供者缺失时退化为全场并集；
4. **视图剔除**：完全落在当前视口之外的源不可能覆盖任何屏幕像素（集合等价，非近似）；
5. **圆盘包含剔除**：源 A 的圆盘完全落在源 B 的圆盘内（`dist + rA <= rB`）时删掉 A 严格无影响（遮罩取 max）。搜索范围限于同一个 HashGrid 格，因此是"可靠的但未必穷尽"；
6. 受 `MaxSceneGpuVisionSources` 截断后交给渲染线程。

同一批数据也按队伍暴露给 CPU 侧消费者：`CollectVisionSourcesByTeam()`，供探索层等逻辑自行累积历史（本插件只维护"当前帧可见性"）。

## 线程模型

- **游戏线程**：`UpdateSceneGpuVisionSources()` 收集清单 → `UploadFrameData_GameThread()` 加锁交换清单与参数快照。游戏线程不碰任何图形资源（旧路径每帧在游戏线程重建一张 RHI 纹理并整块上传）。
- **渲染线程**：`SubscribeToPostProcessingPass()` 取一份快照（多视口时同一帧会多次 Subscribe，因此是拷贝而不是交换），没有源就不注入回调；回调里按 RDG 建缓冲/遮罩纹理并记录两趟 pass。RDG 的 `QueueBufferUpload` 会自己拷贝源数据，因此不存在跨帧生命周期问题。

## 已知取舍

- **投影平面**：视野圆盘固定投影在 `SceneFogWorldPlaneZ` 平面上。像素所在表面高出它 `h` 时，屏幕上会有约 `h / tan(俯仰角)` 的偏移（例如 55° 俯角、200 cm 高度 → 约 140 cm）。这个量级由 `SceneGpuVisionSourceRadiusPadding`（默认 300 cm）覆盖，因此"该看见的仍然看得见"；地形整体抬高时把 `SceneFogWorldPlaneZ` 设成地面高度即可消除偏移。
- **作用范围**：注册的是世界作用域的扩展，因此该世界的**所有视图**都会走这两趟 pass（包括编辑器视口与 SceneCapture）。旧的 `UPostProcessComponent` 已经删除，不再有 `BlendRadius/BlendWeight` 这种"局部生效"的语义。
- **遮罩分辨率**：`SceneGpuVisionMaskResolutionDivisor > 1` 时靠合成的双线性采样软化边界，代价是遮罩边界有约 1 像素的位置量化。
- **平台**：`ShouldCompilePermutation` 限定 SM5（`PF_R8` 渲染目标 + `BO_Max` 混合在移动 GLES 上不保证支持）。

## 测量

- 抓帧看 `FogOfWar.VisionSplat(N)` 与 `FogOfWar.Composite`，前者实例数应等于本帧源数。
- `Saved/Logs/FogOfWar_ScenePerf.csv` 每 `SceneGpuVisionPerformanceLogInterval` 秒一行：
  `WorldTime,Channel,Samples,AvgTotalMs,AvgCollectMs,AvgUploadMs,AvgSourceCount,AvgVisitedCells,AvgVisitedAgents,AvgCulledSources,AvgContainedSources`
  `AvgUploadMs` 现在只记"交接给渲染线程"的耗时（应该是接近 0 的小量）；GPU 侧成本不再由这些 CPU 数字反映，请看抓帧。
