// Copyright Winyunq, 2025. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RHIUtilities.h"
#include "Rendering/RenderingCommon.h"
#include "Widgets/SLeafWidget.h"

struct FMassBattleMinimapGpuTimingState
{
	struct FSample
	{
		explicit FSample(FRHIRenderQueryPool* Pool, const uint32 InAgentCount)
			: TotalBegin(Pool->AllocateQuery())
			, TotalEnd(Pool->AllocateQuery())
			, UnitsBegin(Pool->AllocateQuery())
			, UnitsEnd(Pool->AllocateQuery())
			, VisionBegin(Pool->AllocateQuery())
			, VisionEnd(Pool->AllocateQuery())
			, FogBegin(Pool->AllocateQuery())
			, FogEnd(Pool->AllocateQuery())
			, AgentCount(InAgentCount)
		{
		}

		FRHIPooledRenderQuery TotalBegin;
		FRHIPooledRenderQuery TotalEnd;
		FRHIPooledRenderQuery UnitsBegin;
		FRHIPooledRenderQuery UnitsEnd;
		FRHIPooledRenderQuery VisionBegin;
		FRHIPooledRenderQuery VisionEnd;
		FRHIPooledRenderQuery FogBegin;
		FRHIPooledRenderQuery FogEnd;
		FGraphEventRef RHIEndFence;
		uint32 AgentCount = 0;
	};

	static constexpr uint32 SampleEveryNDraws = 15;
	static constexpr uint32 SamplesPerReport = 16;

	bool ShouldCreateSample(const uint32 AgentCount)
	{
		GatherReadySamples(false);
		++DrawCounter;
		return AgentCount > 0
			&& GSupportsTimestampRenderQueries
			&& (DrawCounter % SampleEveryNDraws) == 1;
	}

	TSharedPtr<FSample, ESPMode::ThreadSafe> CreateSample(const uint32 AgentCount)
	{
		if (!QueryPool.IsValid())
		{
			QueryPool = RHICreateRenderQueryPool(RQT_AbsoluteTime, SamplesPerReport * 8);
		}

		TSharedPtr<FSample, ESPMode::ThreadSafe> Sample =
			MakeShared<FSample, ESPMode::ThreadSafe>(QueryPool.GetReference(), AgentCount);
		PendingSamples.Add(Sample);
		return Sample;
	}

	void Release_RenderThread()
	{
		GatherReadySamples(true);
		if (AccumulatedSamples > 0)
		{
			LogAndReset();
		}
		PendingSamples.Reset();
		QueryPool.SafeRelease();
	}

private:
	static bool ReadQuery(const FRHIPooledRenderQuery& Query, uint64& OutValue, const bool bWait)
	{
		return Query.IsValid() && RHIGetRenderQueryResult(Query.GetQuery(), OutValue, bWait);
	}

	void GatherReadySamples(const bool bWait)
	{
		check(IsInRenderingThread());
		while (PendingSamples.Num() > 0)
		{
			const TSharedPtr<FSample, ESPMode::ThreadSafe>& Sample = PendingSamples[0];
			if (!Sample->RHIEndFence)
			{
				return;
			}
			if (!Sample->RHIEndFence->IsComplete())
			{
				if (!bWait)
				{
					return;
				}
				FRHICommandListExecutor::WaitOnRHIThreadFence(Sample->RHIEndFence);
			}

			uint64 TotalBegin = 0;
			uint64 TotalEnd = 0;
			uint64 UnitsBegin = 0;
			uint64 UnitsEnd = 0;
			uint64 VisionBegin = 0;
			uint64 VisionEnd = 0;
			uint64 FogBegin = 0;
			uint64 FogEnd = 0;
			const bool bReady =
				ReadQuery(Sample->TotalBegin, TotalBegin, bWait)
				&& ReadQuery(Sample->TotalEnd, TotalEnd, bWait)
				&& ReadQuery(Sample->UnitsBegin, UnitsBegin, bWait)
				&& ReadQuery(Sample->UnitsEnd, UnitsEnd, bWait)
				&& ReadQuery(Sample->VisionBegin, VisionBegin, bWait)
				&& ReadQuery(Sample->VisionEnd, VisionEnd, bWait)
				&& ReadQuery(Sample->FogBegin, FogBegin, bWait)
				&& ReadQuery(Sample->FogEnd, FogEnd, bWait);

			if (!bReady)
			{
				if (!bWait)
				{
					return;
				}
				PendingSamples.RemoveAt(0);
				continue;
			}

			const double UnitsMs = static_cast<double>(UnitsEnd - UnitsBegin) / 1000.0;
			const double VisionMs = static_cast<double>(VisionEnd - VisionBegin) / 1000.0;
			const double FogMs = static_cast<double>(FogEnd - FogBegin) / 1000.0;
			const double TotalMs = static_cast<double>(TotalEnd - TotalBegin) / 1000.0;
			Accumulate(Sample->AgentCount, UnitsMs, VisionMs, FogMs, TotalMs);
			PendingSamples.RemoveAt(0);
		}
	}

	void Accumulate(
		const uint32 AgentCount,
		const double UnitsMs,
		const double VisionMs,
		const double FogMs,
		const double TotalMs)
	{
		if (AccumulatedSamples > 0 && TimedAgentCount != AgentCount)
		{
			LogAndReset();
		}

		TimedAgentCount = AgentCount;
		++AccumulatedSamples;
		UnitsSumMs += UnitsMs;
		VisionSumMs += VisionMs;
		FogSumMs += FogMs;
		TotalSumMs += TotalMs;
		TotalMinMs = FMath::Min(TotalMinMs, TotalMs);
		TotalMaxMs = FMath::Max(TotalMaxMs, TotalMs);

		if (AccumulatedSamples >= SamplesPerReport)
		{
			LogAndReset();
		}
	}

	void LogAndReset()
	{
		const double InvSamples = 1.0 / static_cast<double>(AccumulatedSamples);
		UE_LOG(LogTemp, Display,
			TEXT("MassBattleMinimapPerf GPU: Agents=%u Samples=%u UnitsAvg=%.3fms VisionAvg=%.3fms FogAvg=%.3fms TotalAvg=%.3fms TotalMin=%.3fms TotalMax=%.3fms"),
			TimedAgentCount,
			AccumulatedSamples,
			UnitsSumMs * InvSamples,
			VisionSumMs * InvSamples,
			FogSumMs * InvSamples,
			TotalSumMs * InvSamples,
			TotalMinMs,
			TotalMaxMs);

		AccumulatedSamples = 0;
		UnitsSumMs = 0.0;
		VisionSumMs = 0.0;
		FogSumMs = 0.0;
		TotalSumMs = 0.0;
		TotalMinMs = TNumericLimits<double>::Max();
		TotalMaxMs = 0.0;
	}

	FRenderQueryPoolRHIRef QueryPool;
	TArray<TSharedPtr<FSample, ESPMode::ThreadSafe>> PendingSamples;
	uint64 DrawCounter = 0;
	uint32 TimedAgentCount = 0;
	uint32 AccumulatedSamples = 0;
	double UnitsSumMs = 0.0;
	double VisionSumMs = 0.0;
	double FogSumMs = 0.0;
	double TotalSumMs = 0.0;
	double TotalMinMs = TNumericLimits<double>::Max();
	double TotalMaxMs = 0.0;
};

struct FMassBattleMinimapUploadData
{
	TArray<FVector> Locations;
	TArray<FVector4f> DynamicParams0;
	TArray<bool> IsHidden;
	TArray<FLinearColor> TeamColors;
	FVector2f MapMin = FVector2f::ZeroVector;
	FVector2f MapSize = FVector2f(1.0f, 1.0f);
	int32 LogicalResolution = 256;
	float VisionRadiusUU = 4000.0f;
	float UnitRadiusUU = 100.0f;
	float FogOpacity = 0.5f;
	uint32 ViewingTeamIndex = 0;
};

/** Persistent read-only GPU buffers. They are replaced only at the minimap update cadence. */
class FMassBattleMinimapRenderData final
	: public TSharedFromThis<FMassBattleMinimapRenderData, ESPMode::ThreadSafe>
{
public:
	~FMassBattleMinimapRenderData();

	void Upload_GameThread(FMassBattleMinimapUploadData&& UploadData);
	void Release_GameThread();
	void Draw_RenderThread(
		FRDGBuilder& GraphBuilder,
		const ICustomSlateElement::FDrawPassInputs& Inputs,
		const FPaintGeometry& PaintGeometry);

private:
	void Upload_RenderThread(FRHICommandListImmediate& RHICmdList, const FMassBattleMinimapUploadData& UploadData);
	void Release_RenderThread();

	FReadBuffer LocationWordsBuffer;
	FReadBuffer DynamicParams0Buffer;
	FReadBuffer IsHiddenBuffer;
	FReadBuffer TeamColorsBuffer;

	FVector2f MapMin_RenderThread = FVector2f::ZeroVector;
	FVector2f MapSize_RenderThread = FVector2f(1.0f, 1.0f);
	uint32 LogicalResolution_RenderThread = 256;
	float VisionRadiusUU_RenderThread = 4000.0f;
	float UnitRadiusUU_RenderThread = 100.0f;
	float FogOpacity_RenderThread = 0.5f;
	uint32 ViewingTeamIndex_RenderThread = 0;
	uint32 AgentCount_RenderThread = 0;
	uint32 TeamColorCount_RenderThread = 0;
	TUniquePtr<FMassBattleMinimapGpuTimingState> GpuTimingState;
};

using FMassBattleMinimapRenderDataPtr = TSharedPtr<FMassBattleMinimapRenderData, ESPMode::ThreadSafe>;

class SMassBattleFrameMinimap final : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SMassBattleFrameMinimap) {}
		SLATE_ARGUMENT(FMassBattleMinimapRenderDataPtr, RenderData)
	SLATE_END_ARGS()

	virtual ~SMassBattleFrameMinimap() override;
	void Construct(const FArguments& InArgs);

	virtual int32 OnPaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;

private:
	FMassBattleMinimapRenderDataPtr RenderData;
	TSharedPtr<ICustomSlateElement, ESPMode::ThreadSafe> CustomDrawer;
};
