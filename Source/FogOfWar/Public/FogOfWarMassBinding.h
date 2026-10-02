// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#ifndef FOW_USE_MASSBATTLE_BINDING
#define FOW_USE_MASSBATTLE_BINDING 1
#endif

#if FOW_USE_MASSBATTLE_BINDING
#include "Fragments/Team.h"
#include "Fragments/Transform.h"
#include "Fragments/Trace.h"
#include "MassBattleEnums.h"

#define FOW_LOCATION_FRAGMENT FLocating
#define FOW_TEAM_FRAGMENT FTeam
#define FOW_GET_LOCATION(Fragment) ((Fragment).Location)
#define FOW_GET_PREVIOUS_LOCATION(Fragment) ((Fragment).PreLocation)
#define FOW_GET_TEAM_INDEX(Fragment) ((Fragment).index)

// 朝向绑定：视野扇形的朝向 = 单位朝向（FRotating::Direction，已是 2D 单位向量）。
// 扇形揭雾需要"往哪看"，而位置/队伍都提供不了它，所以单独绑一个朝向来源。
#define FOW_ROTATION_FRAGMENT FRotating
#define FOW_GET_ROTATION_DIRECTION(Fragment) ((Fragment).Direction)
#define FOW_HAS_MASSBATTLE_ROTATION 1

// 索敌配置绑定：揭雾半径按 FTrace::Mode 分派到该模式实际使用的索敌参数
// （解析实现在 FogOfWarVisionRadius.cpp，模式到数据结构的映射表见该文件）。
#define FOW_HAS_MASSBATTLE_TRACE 1

#else
#include "MassFogOfWarFragments.h"

#define FOW_LOCATION_FRAGMENT FFogOfWarLocationFragment
#define FOW_TEAM_FRAGMENT FFogOfWarTeamFragment
#define FOW_GET_LOCATION(Fragment) ((Fragment).Location)
#define FOW_GET_PREVIOUS_LOCATION(Fragment) ((Fragment).PreviousLocation)
#define FOW_GET_TEAM_INDEX(Fragment) ((Fragment).TeamIndex)

// 无 MassBattle 绑定时没有索敌配置可读，揭雾半径只能取配置默认值。
#define FOW_HAS_MASSBATTLE_TRACE 0

// 无 MassBattle 绑定时也拿不到单位朝向：所有视野源按"全向"处理（行为等同旧的圆形雾）。
#define FOW_HAS_MASSBATTLE_ROTATION 0

#endif
