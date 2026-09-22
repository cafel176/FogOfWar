// Copyright Winyunq, 2026. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWorld;

/**
 * 由“地图区域导出器”与“运行时小地图”共同使用的精确资源路径解析工具。
 *
 * 关键点：一个关卡的短名（Short Name）绝不能直接作为配置键使用，因为不同目录
 * 可能同名。本工具用规范的包路径（含目录）为每个关卡唯一确定配置目录与 ini 路径，
 * 避免跨地图串数据。
 */
namespace MassBattleMapProfilePaths
{
	// 返回给定 World 的规范地图包路径（如 /Game/Maps/MyMap）；无 World 时回退 /Default。
	FOGOFWAR_API FString GetCanonicalMapPackagePath(const UWorld* World);
	// 返回该地图专属配置目录（Project/Config/MapRegion/<规范包路径>）。
	FOGOFWAR_API FString GetConfigDirectory(const UWorld* World);
	// 返回该地图的 MapRegion.ini 完整路径。
	FOGOFWAR_API FString GetMapRegionIniPath(const UWorld* World);
	// 返回该地图的 MinimapColors.ini 完整路径。
	FOGOFWAR_API FString GetMinimapColorsIniPath(const UWorld* World);
	// 返回该地图的 MinimapBackground.ini 完整路径。
	FOGOFWAR_API FString GetMinimapBackgroundIniPath(const UWorld* World);
}
