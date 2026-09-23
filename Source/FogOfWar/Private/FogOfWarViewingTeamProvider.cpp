// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWarViewingTeamProvider.h"

/// 提供者的唯一存储：仅本模块内可见，外部只能经 Set / Reset / IsRegistered / GetViewingTeam 访问。
static FFogOfWarGetViewingTeamDelegate GFogOfWarViewingTeamProvider;

void FFogOfWarViewingTeamProvider::Set(const FFogOfWarGetViewingTeamDelegate& InDelegate)
{
	GFogOfWarViewingTeamProvider = InDelegate;
}

void FFogOfWarViewingTeamProvider::Reset()
{
	GFogOfWarViewingTeamProvider.Unbind();
}

bool FFogOfWarViewingTeamProvider::IsRegistered()
{
	return GFogOfWarViewingTeamProvider.IsBound();
}

int32 FFogOfWarViewingTeamProvider::GetViewingTeam(const UObject* WorldContextObject)
{
	// 未注册：说明没有外部系统在提供观察队伍（只启用了本插件，或注册方模块尚未启动）——
	// 返回 INDEX_NONE 而不是 0，避免被误当成“观察队伍 0”而把其他队伍的视野源全部过滤掉。
	if (!GFogOfWarViewingTeamProvider.IsBound())
	{
		return INDEX_NONE;
	}

	// 注册方返回负值同样归一为 INDEX_NONE：本类对外的契约只有“合法队伍下标 / INDEX_NONE”两种结果。
	const int32 ViewingTeamIndex = GFogOfWarViewingTeamProvider.Execute(WorldContextObject);
	return ViewingTeamIndex >= 0 ? ViewingTeamIndex : INDEX_NONE;
}
