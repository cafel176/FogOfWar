// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWarExploredLayerProvider.h"

/// 提供者的唯一存储：仅本模块内可见，外部只能经 Set / Reset / IsRegistered / GetExploredLayer 访问。
static FFogOfWarGetExploredLayerDelegate GFogOfWarExploredLayerProvider;

void FFogOfWarExploredLayerProvider::Set(const FFogOfWarGetExploredLayerDelegate& InDelegate)
{
	GFogOfWarExploredLayerProvider = InDelegate;
}

void FFogOfWarExploredLayerProvider::Reset()
{
	GFogOfWarExploredLayerProvider.Unbind();
}

bool FFogOfWarExploredLayerProvider::IsRegistered()
{
	return GFogOfWarExploredLayerProvider.IsBound();
}

bool FFogOfWarExploredLayerProvider::GetExploredLayer(int32 TeamIndex, FFogOfWarExploredLayerView& OutView)
{
	// 没有明确的观察队伍就没有明确的探索层：探索层是逐队累积的，随便挑一支队伍的历史
	// 会在观察队伍切换时把另一支队伍的灰雾画出来 —— 宁可退化到二态雾。
	if (TeamIndex == INDEX_NONE)
	{
		return false;
	}

	if (!GFogOfWarExploredLayerProvider.IsBound())
	{
		return false;
	}

	FFogOfWarExploredLayerView View;
	if (!GFogOfWarExploredLayerProvider.Execute(TeamIndex, View) || !View.IsValid())
	{
		return false;
	}

	OutView = View;
	return true;
}
