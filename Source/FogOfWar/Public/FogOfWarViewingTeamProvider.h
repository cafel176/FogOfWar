// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/// @file FogOfWarViewingTeamProvider.h
/// @brief “当前观察队伍”提供者的注册点：本插件不反向依赖任何业务模块，由外部把队伍查询函数注册进来。

/**
 * @brief 观察队伍查询委托。
 * @details 入参与返回值口径与 UMassBattleGlobalVarFunctionLibrary::GetTeam 完全一致：
 *          入参为世界上下文对象，返回当前观察队伍下标；返回负值（INDEX_NONE）表示“队伍不可用”。
 */
DECLARE_DELEGATE_RetVal_OneParam(int32, FFogOfWarGetViewingTeamDelegate, const UObject* /* WorldContextObject */);

/**
 * @class FFogOfWarViewingTeamProvider
 * @brief 观察队伍提供者的进程内注册点（全局唯一一份）。
 * @details 本插件只声明“我需要知道当前观察队伍是谁”，不关心它来自哪个模块 ——
 *          直接调用 UMassBattleGlobalVarFunctionLibrary::GetTeam 会让本插件反向依赖 MassBattleSystem，
 *          与该模块的依赖方向（MassBattleSystem → MassBattleMap → FogOfWar）相冲突。
 *          因此改为：外部（MassBattleSystem 模块启动时）把该函数注册进来，本插件只经本类查询。
 *          未注册、或注册方返回负值时，查询结果为 INDEX_NONE，调用方按“队伍不可用”处理
 *          （场景视野源退化为不按队伍过滤，避免整屏变黑）。
 */
struct FOGOFWAR_API FFogOfWarViewingTeamProvider
{
	/**
	 * @brief       注册（或覆盖）观察队伍提供者。
	 * @details     进程内只应有一个注册方；重复注册以最后一次为准。
	 * @param       InDelegate  提供者委托；传空委托等价于 Reset()。
	 */
	static void Set(const FFogOfWarGetViewingTeamDelegate& InDelegate);

	/**
	 * @brief       注销提供者。
	 * @details     模块卸载（含 Live Coding 重编译）时调用，避免留下指向已卸载代码的函数指针。
	 */
	static void Reset();

	/**
	 * @brief       当前是否已注册提供者。
	 * @return      bool
	 * @retval      true 表示已注册。
	 */
	static bool IsRegistered();

	/**
	 * @brief       查询当前观察队伍下标。
	 * @param       WorldContextObject  世界上下文（由注册方解释，通常是 World 或 World 内的任意对象）。
	 * @return      int32
	 * @retval      队伍下标；未注册或注册方返回负值时返回 INDEX_NONE。
	 */
	static int32 GetViewingTeam(const UObject* WorldContextObject);
};
