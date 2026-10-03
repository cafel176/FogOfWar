// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/// @file FogOfWarCanDrawFogProvider.h
/// @brief “本帧是否允许绘制迷雾”的注册点：本插件不反向依赖任何业务模块，由外部把判定函数注册进来。

/**
 * @brief “是否允许绘制迷雾”查询委托。
 *
 * @details 无入参。本工程的实现是 UMassBattleGlobalVarFunctionLibrary::CanDrawFog ——
 *          它按全局游戏状态（EGlobalGameState）判定：编辑场景与主菜单不画，模拟 / 训练 / 调试画。
 *          这里刻意不留世界上下文参数，正是为了让注册方可以**直接**绑那个函数、两边形态完全一致
 *          （少一层适配 lambda 就少一处将来会漂移的地方）。
 */
DECLARE_DELEGATE_RetVal(bool, FFogOfWarCanDrawFogDelegate);

/**
 * @class FFogOfWarCanDrawFogProvider
 * @brief “是否允许绘制迷雾”的进程内注册点（全局唯一一份）。
 *
 * @details 与 FFogOfWarViewingTeamProvider 同一个范式：本插件只声明“我需要知道现在能不能画雾”，
 *          不关心它来自哪个模块 —— 直接调用 UMassBattleGlobalVarFunctionLibrary 会让本插件
 *          反向依赖 MassBattleSystem，与该模块的依赖方向（MassBattleSystem → MassBattleMap → FogOfWar）
 *          冲突。因此改为：外部（MassBattleSystem 模块启动时）把该函数注册进来，本插件只经本类查询。
 *
 * @details 它是**第二道闸门**，与 AFogOfWar::bEnableSceneGpuVisionSources 串成"与"的关系：
 *            · bEnableSceneGpuVisionSources —— 本插件自己的总开关（"我这次不想用这个插件"）；
 *            · 本提供者 —— 业务侧按游戏状态给出的裁决（"现在这个状态下不该有雾"）。
 *          两者语义不同，都要留。
 */
struct FOGOFWAR_API FFogOfWarCanDrawFogProvider
{
	/**
	 * @brief 注册（或覆盖）判定函数。
	 * @details 进程内只应有一个注册方；重复注册以最后一次为准。
	 * @param InDelegate 判定委托；传空委托等价于 Reset()。
	 */
	static void Set(const FFogOfWarCanDrawFogDelegate& InDelegate);

	/**
	 * @brief 注销。
	 * @details 模块卸载（含 Live Coding 重编译）时调用，避免留下指向已卸载代码的函数指针。
	 */
	static void Reset();

	/** @brief 当前是否已注册判定函数。 */
	static bool IsRegistered();

	/**
	 * @brief 本帧是否允许绘制迷雾。
	 *
	 * @return true = 允许绘制。
	 * @note **未注册时返回 true**：本插件独立使用（没有业务模块接管游戏状态）时的默认行为一直是
	 *       "画雾"。把"没注册"解释成"不许画"会让插件静默失效（画面没有任何雾、也没有任何报错），
	 *       而那从来不是任何人的意图 —— 与本插件其它提供者的退化取向一致：缺契约时不生效的是**限制**，
	 *       而不是功能本身。
	 */
	static bool CanDrawFog();
};
