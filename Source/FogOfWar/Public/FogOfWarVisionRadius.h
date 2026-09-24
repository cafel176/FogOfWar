// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FogOfWarMassBinding.h"
#include "MassEntityTypes.h"
#include "MassEntityManager.h"

/**
 * @file FogOfWarVisionRadius.h
 * @brief 揭雾半径解析：把实体自身的索敌(Trace)配置翻译成战争迷雾使用的揭雾半径。
 * @details 战争迷雾的揭雾半径不再使用单一常量，而是从实体的 FTrace 配置中按索敌模式
 *          取出该模式实际生效的索敌参数。模式到数据结构的映射集中在
 *          FogOfWarVisionRadius.cpp 的映射表里，新增索敌模式时只需在该表里补一行。
 */
namespace FogOfWarVision
{
	/**
	 * @brief 解析实体的揭雾半径（单位：厘米）。
	 * @details 按 FTrace::Mode 分派到该模式实际使用的索敌参数结构，取其"通用(Common)"参数
	 *          中的索敌半径：
	 *
	 *          | FTrace::Mode        | 半径来源                                  |
	 *          |---------------------|-------------------------------------------|
	 *          | SectorTraceByTraits | FTrace::SectorTrace::Common::TraceRadius  |
	 *          | TargetIsPlayer_0    | 该模式不使用扇形索敌参数，无对应半径 → 回退 |
	 *
	 *          以下情况一律回退到 InFallbackCm：
	 *          ① 实体没有 FTrace（例如非 MassBattle Agent、用 UMassVisionTrait 手工配置的单位）；
	 *          ② FTrace::bEnable == false（该单位不索敌）；
	 *          ③ 取到的半径 <= 0（配置为 0 表示不揭雾，交由调用方的回退值兜底）；
	 *          ④ FogOfWar 未开启 MassBattle 绑定（FOW_HAS_MASSBATTLE_TRACE == 0）。
	 *
	 * @param InEntityManager 实体管理器（只读）。
	 * @param InEntity        目标实体句柄。
	 * @param InFallbackCm    回退半径，通常传 UMinimapDataSubsystem::DefaultMassBattleSightRadius。
	 * @return 该实体的揭雾半径（厘米）；无法从索敌配置解析时返回 InFallbackCm。
	 */
	FOGOFWAR_API float ResolveSightRadiusCm(
		const FMassEntityManager& InEntityManager,
		const FMassEntityHandle InEntity,
		float InFallbackCm);
}
