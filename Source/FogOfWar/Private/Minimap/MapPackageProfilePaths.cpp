// Copyright Winyunq, 2026. All Rights Reserved.

#include "Minimap/MapPackageProfilePaths.h"

#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace MassBattleMapProfilePaths
{
	/**
	 * 计算规范地图包路径。步骤：① 无 World 回退 /Default；
	 * ② 去流送关卡前缀，取地图短名；
	 * ③ 取包所在目录；④ 无目录则 /<名字>，否则 <目录>/<名字>。
	 */
	FString GetCanonicalMapPackagePath(const UWorld* World)
	{
		if (!World)
		{
			return TEXT("/Default");
		}

		// 去流送前缀并取包目录。
		FString MapName = World->GetName();
		MapName.RemoveFromStart(World->StreamingLevelsPrefix);
		const FString PackageDirectory = FPackageName::GetLongPackagePath(
			World->GetPackage()->GetName());
		return PackageDirectory.IsEmpty() ? TEXT("/") + MapName : PackageDirectory / MapName;
	}

	/** 取该地图的配置目录：规范包路径去掉前导 / 后拼到 Project/Config/MapRegion 下。 */
	FString GetConfigDirectory(const UWorld* World)
	{
		FString RelativePackagePath = GetCanonicalMapPackagePath(World);
		RelativePackagePath.RemoveFromStart(TEXT("/"));
		return FPaths::ProjectConfigDir() / TEXT("MapRegion") / RelativePackagePath;
	}

	// 返回该地图的 MapRegion.ini 路径。
	FString GetMapRegionIniPath(const UWorld* World)
	{
		return GetConfigDirectory(World) / TEXT("MapRegion.ini");
	}

	// 返回该地图的 MinimapColors.ini 路径。
	FString GetMinimapColorsIniPath(const UWorld* World)
	{
		return GetConfigDirectory(World) / TEXT("MinimapColors.ini");
	}

	// 返回该地图的 MinimapBackground.ini 路径。
	FString GetMinimapBackgroundIniPath(const UWorld* World)
	{
		return GetConfigDirectory(World) / TEXT("MinimapBackground.ini");
	}
}
