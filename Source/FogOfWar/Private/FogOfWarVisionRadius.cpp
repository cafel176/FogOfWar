// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWarVisionRadius.h"

#include "MassEntityManager.h"

#if FOW_HAS_MASSBATTLE_TRACE
#include "Fragments/Trace.h"
#include "MassBattleEnums.h"
#endif

namespace FogOfWarVision
{
#if FOW_HAS_MASSBATTLE_TRACE
	namespace
	{
		/**
		 * 索敌模式 → 该模式实际生效的揭雾半径（映射表）。
		 * @param InTrace 实体的索敌配置。
		 * @return 半径（厘米）；<= 0 表示该模式没有与迷雾对应的半径，调用方回退默认值。
		 */
		float GetModeSightRadiusCm(const FTrace& InTrace)
		{
			switch (InTrace.Mode)
			{
			case ETraceMode::SectorTraceByTraits:
				// 按特征扇形索敌：范围由"通用(Common)"索敌参数给出，
				// 因此揭雾半径直接取 Common::TraceRadius。
				// 注：行为状态(Sleep/Patrol/Chase/Reinforce)的专用参数只影响索敌判定，
				//     不参与揭雾半径，避免视野圈随行为状态跳变。
				return InTrace.SectorTrace.Common.TraceRadius;

			case ETraceMode::TargetIsPlayer_0:
				// 目标为玩家的模式不枚举扇形范围（直接对玩家实体做判定），
				// 没有与迷雾对应的索敌半径。
				return 0.0f;

			default:
				return 0.0f;
			}
		}

		/**
		 * 索敌模式 → 该模式实际生效的揭雾张角（映射表；与半径表一一对应）。
		 * @param InTrace 实体的索敌配置。
		 * @return 张角（度，360 = 全向）；<= 0 表示该模式没有与迷雾对应的张角，调用方回退默认值。
		 */
		float GetModeSightAngleDegrees(const FTrace& InTrace)
		{
			switch (InTrace.Mode)
			{
			case ETraceMode::SectorTraceByTraits:
				// 与半径取自同一个 Common 参数结构：揭雾扇形与索敌扇形共用一组配置，
				// 不会出现"半径用 Common、角度用别处"的错配。
				return InTrace.SectorTrace.Common.TraceAngle;

			case ETraceMode::TargetIsPlayer_0:
				// 目标为玩家的模式不做扇形范围枚举，没有对应的张角。
				return 0.0f;

			default:
				return 0.0f;
			}
		}
	}
#endif // FOW_HAS_MASSBATTLE_TRACE

	float ResolveSightRadiusCm(
		const FMassEntityManager& InEntityManager,
		const FMassEntityHandle InEntity,
		float InFallbackCm)
	{
#if FOW_HAS_MASSBATTLE_TRACE
		if (InEntityManager.IsEntityValid(InEntity))
		{
			const FTrace* TraceFragment = InEntityManager.GetFragmentDataPtr<FTrace>(InEntity);
			if (TraceFragment && TraceFragment->bEnable)
			{
				const float ModeRadiusCm = GetModeSightRadiusCm(*TraceFragment);
				if (ModeRadiusCm > 0.0f)
				{
					return ModeRadiusCm;
				}
			}
		}
#endif // FOW_HAS_MASSBATTLE_TRACE

		return InFallbackCm;
	}

	float ResolveSightAngleDegrees(
		const FMassEntityManager& InEntityManager,
		const FMassEntityHandle InEntity,
		float InFallbackDegrees)
	{
#if FOW_HAS_MASSBATTLE_TRACE
		if (InEntityManager.IsEntityValid(InEntity))
		{
			const FTrace* TraceFragment = InEntityManager.GetFragmentDataPtr<FTrace>(InEntity);
			if (TraceFragment && TraceFragment->bEnable)
			{
				const float ModeAngleDegrees = GetModeSightAngleDegrees(*TraceFragment);
				if (ModeAngleDegrees > 0.0f)
				{
					return ModeAngleDegrees;
				}
			}
		}
#endif // FOW_HAS_MASSBATTLE_TRACE

		// 没有索敌配置 / 该模式无张角 / 未开启 MassBattle 绑定 → 全向。
		// 调用方传 360，渲染侧据此走"圆盘"分支，行为与旧的圆形揭雾完全一致。
		return InFallbackDegrees;
	}
}
