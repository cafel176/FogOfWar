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
}
