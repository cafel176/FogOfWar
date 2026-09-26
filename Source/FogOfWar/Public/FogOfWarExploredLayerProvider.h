// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/// @file FogOfWarExploredLayerProvider.h
/// @brief “历史已探索层”提供者的注册点：本插件不反向依赖任何业务模块，由外部把探索层的只读视图注册进来。

/**
 * @struct FFogOfWarExploredLayerView
 * @brief 一份“逐格已探索”位图的只读视图 + 它所在的世界矩形。
 *
 * @details 这是“记忆视野通道”的数据来源：本插件只维护“当前帧可见性”，不保存任何历史；
 *          历史由外部系统（本工程里是 UMassBattleMapSubsystem 的逐队探索层）持有，
 *          它通过本结构把位图借给渲染侧。位图的所有权仍在提供者手里，本插件只在本帧内读，
 *          不缓存指针 —— 因此提供者不需要为每次查询准备稳定的存储。
 *
 * @details 世界矩形必须与视野场用的是同一个（都是地图网格范围）：G 通道的采样 UV 直接沿用场的 UV，
 *          两边矩形不一致会让灰雾整体错位。
 */
struct FFogOfWarExploredLayerView
{
	/**
	 * @brief 稠密位图：行主序（index = y * Width + x），bit 序 LSB-first，
	 *        字节数 = ceil(Width * Height / 8)。
	 * @note 只有“已探索 / 未探索”两态，因此用位图而不是字节数组：200×200 的图只有 5KB。
	 */
	const uint8* Bitmap = nullptr;

	/// @brief 位图宽度（格）。
	int32 Width = 0;

	/// @brief 位图高度（格）。
	int32 Height = 0;

	/// @brief 位图覆盖的世界矩形最小角（XY，cm）。
	FVector2D WorldMin = FVector2D::ZeroVector;

	/// @brief 位图覆盖的世界矩形尺寸（XY，cm）。
	FVector2D WorldSize = FVector2D::ZeroVector;

	/**
	 * @brief 位图内容版本号：只有它变化时才需要重新展开并上传。
	 * @details 探索层每秒只变几次，而渲染每帧都要问一次；没有版本号就只能每帧搬一遍位图，
	 *          白白多出几十 KB 的拷贝与上传。
	 */
	int32 Version = INDEX_NONE;

	/** 视图是否可用（字段自洽）。 */
	FORCEINLINE bool IsValid() const
	{
		return Bitmap != nullptr
			&& Width > 0 && Height > 0
			&& WorldSize.X > 0.0f && WorldSize.Y > 0.0f;
	}
};

/**
 * @brief 已探索层查询委托。
 * @details 入参为队伍下标（与 FFogOfWarViewingTeamProvider 同口径），
 *          返回是否成功填充 OutView；返回 false 表示“该队伍此刻没有可用的探索层”。
 */
DECLARE_DELEGATE_RetVal_TwoParams(bool, FFogOfWarGetExploredLayerDelegate, int32 /* TeamIndex */, FFogOfWarExploredLayerView& /* OutView */);

/**
 * @class FFogOfWarExploredLayerProvider
 * @brief 已探索层提供者的进程内注册点（全局唯一一份）。
 *
 * @details 本插件只声明“我需要观察队伍已经探索过哪些格”，不关心它来自哪个模块 ——
 *          直接读 MassBattleMap 会让本插件反向依赖 MassBattleAgent，与该模块的依赖方向
 *          （MassBattleSystem → MassBattleMap → FogOfWar）相冲突。
 *          因此改为：外部（MassBattleMap subsystem 初始化时）把查询委托注册进来，本插件只经本类查询。
 *
 * @details 未注册时渲染侧退化为阶段一的二态雾（只有“可见 / 不可见”），不会报错也不会停摆：
 *          “没有历史”是一个合法状态，而不是失败。
 */
struct FOGOFWAR_API FFogOfWarExploredLayerProvider
{
	/**
	 * @brief       注册（或覆盖）已探索层提供者。
	 * @details     进程内只应有一个注册方；重复注册以最后一次为准。
	 * @param       InDelegate  提供者委托；传空委托等价于 Reset()。
	 */
	static void Set(const FFogOfWarGetExploredLayerDelegate& InDelegate);

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
	 * @brief       查询指定队伍的已探索层。
	 * @param       TeamIndex  队伍下标；INDEX_NONE 表示“队伍不可用”，直接返回 false
	 *                         （没有明确的队伍就没有明确的探索层，宁可不画灰雾，也不借用别的队伍的位）。
	 * @param       OutView    输出：位图视图；失败时不修改。
	 * @return      bool
	 * @retval      true 表示 OutView 已填充且可用。
	 */
	static bool GetExploredLayer(int32 TeamIndex, FFogOfWarExploredLayerView& OutView);
};
