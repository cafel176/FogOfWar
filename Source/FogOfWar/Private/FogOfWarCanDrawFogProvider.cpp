// Copyright Winyunq, 2025. All Rights Reserved.

#include "FogOfWarCanDrawFogProvider.h"

/// 判定函数的唯一存储：仅本模块内可见，外部只能经 Set / Reset / IsRegistered / CanDrawFog 访问。
static FFogOfWarCanDrawFogDelegate GFogOfWarCanDrawFogDelegate;

void FFogOfWarCanDrawFogProvider::Set(const FFogOfWarCanDrawFogDelegate& InDelegate)
{
	GFogOfWarCanDrawFogDelegate = InDelegate;
}

void FFogOfWarCanDrawFogProvider::Reset()
{
	GFogOfWarCanDrawFogDelegate.Unbind();
}

bool FFogOfWarCanDrawFogProvider::IsRegistered()
{
	return GFogOfWarCanDrawFogDelegate.IsBound();
}

bool FFogOfWarCanDrawFogProvider::CanDrawFog()
{
	// 未注册：按"允许"处理（见头文件的说明）。注意这里**不能**返回 false ——
	// 那会把"没人告诉我"与"明确不许画"混为一谈，让插件在独立使用时静默失效。
	return GFogOfWarCanDrawFogDelegate.IsBound() ? GFogOfWarCanDrawFogDelegate.Execute() : true;
}
