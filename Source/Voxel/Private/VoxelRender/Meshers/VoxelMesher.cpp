// Copyright 2020 Phyronnaz

#include "VoxelRender/Meshers/VoxelMesher.h"
#include "VoxelRender/VoxelMesherAsyncWork.h"
#include "VoxelRender/VoxelChunkMesh.h"
#include "VoxelRender/IVoxelRenderer.h"
#include "VoxelData/VoxelData.h"
#include "VoxelDebug/VoxelDebugManager.h"
#include "VoxelStatsUtilities.h"

#include "Async/Async.h"
#include "Engine/World.h"

static TAutoConsoleVariable<int32> CVarDoNotSkipEmptyChunks(
	TEXT("voxel.mesher.DoNotSkipEmptyChunks"),
	0,
	TEXT("If true, all chunks will be computed"),
	ECVF_Default);

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

struct FVoxelMesherStats
{
	static FVoxelMesherStats Singleton;

	struct FChunkStats
	{
		int32 LOD = 0;
		double Time = 0;
		FVoxelMesherTimes Times;
	};

	FCriticalSection Section;
	struct FStats
	{
		TArray<FChunkStats> NormalStats;
		TArray<FChunkStats> TransitionsStats;
		TArray<FChunkStats> GeometryStats;
	};
	TMap<TWeakObjectPtr<UWorld>, FStats> StatsMap;

	static void Clean()
	{
		Singleton.StatsMap.Remove(nullptr);
	}
	static void Report(TWeakObjectPtr<UWorld> World, int32 LOD, double TotalTime, const FVoxelMesherTimes& Times, bool bIsTransition, bool bIsGeometry)
	{
#if ENABLE_MESHER_STATS
		check(!bIsGeometry || !bIsTransition);
		FScopeLock Lock(&Singleton.Section);
		auto& LocalStats = Singleton.StatsMap.FindOrAdd(World);
		(bIsGeometry ? LocalStats.GeometryStats : bIsTransition ? LocalStats.TransitionsStats : LocalStats.NormalStats).Add(FChunkStats{ LOD, TotalTime, Times });
#endif
	}
	static void Clear(UWorld* World)
	{
		Clean();

		FScopeLock Lock(&Singleton.Section);
		auto& LocalStats = Singleton.StatsMap.FindOrAdd(World);
		LocalStats.NormalStats.Empty();
		LocalStats.TransitionsStats.Empty();
	}
	static void PrintStats(UWorld* World)
	{
		Clean();
		
		FScopeLock Lock(&Singleton.Section);

		double TotalValuesTime = 0;
		double TotalMaterialsTime = 0;
		uint64 TotalValuesAccesses = 0;
		uint64 TotalMaterialsAccesses = 0;
		
		const auto Print = [&](const TArray<FChunkStats>& Stats)
		{
			struct FMean
			{
				double TotalTime = 0;
				int32 Count = 0;

				double ValuesTime = 0;
				double MaterialsTime = 0;
				
				double NormalsTime = 0;
				double UVsTime = 0;
				double CreateChunkTime = 0;

				uint64 ValuesAccesses = 0;
				uint64 MaterialsAccesses = 0;
			};
			TMap<int32, FMean> LODToMeans;
			double GlobalTotalTime = 0;
			for (auto& Stat : Stats)
			{
				auto& Mean = LODToMeans.FindOrAdd(Stat.LOD);
				Mean.TotalTime += Stat.Time;
				Mean.Count++;

				Mean.ValuesTime += FPlatformTime::ToSeconds64(Stat.Times._Values);
				Mean.MaterialsTime += FPlatformTime::ToSeconds64(Stat.Times._Materials);

				Mean.NormalsTime += FPlatformTime::ToSeconds64(Stat.Times.Normals);
				Mean.UVsTime += FPlatformTime::ToSeconds64(Stat.Times.UVs);
				Mean.CreateChunkTime += FPlatformTime::ToSeconds64(Stat.Times.CreateChunk);

				Mean.ValuesAccesses += Stat.Times._ValuesAccesses;
				Mean.MaterialsAccesses += Stat.Times._MaterialsAccesses;
				
				GlobalTotalTime += Stat.Time;
			}

			LODToMeans.KeySort(TLess<int32>());

			UE_LOG(LogVoxel, Log, TEXT("\tLOD; Chunks (%%)     ; Total (%%)         ; Avg       ; Values (%%)        , Per Voxel ; Materials (%%)     , Per Voxel ; Normals (%%)       ; UVs (%%)           ; CreateChunk (%%)   ;"));
			for (auto& It : LODToMeans)
			{
				auto& V = It.Value;
				
				TotalValuesTime += V.ValuesTime;
				TotalMaterialsTime += V.MaterialsTime;
				TotalValuesAccesses += V.ValuesAccesses;
				TotalMaterialsAccesses += V.MaterialsAccesses;
				
				UE_LOG(LogVoxel, Log, TEXT("\t %2d: %6d (%5.2f%%); %8.3fs (%5.2f%%); %8.3fms; %8.3fs (%5.2f%%), %8.1fns; %8.3fs (%5.2f%%), %8.1fns; %8.3fs (%5.2f%%); %8.3fs (%5.2f%%); %8.3fs (%5.2f%%)"),
					It.Key,
					V.Count,
					V.Count / double(Stats.Num()) * 100,
					V.TotalTime,
					V.TotalTime / GlobalTotalTime * 100,
					V.TotalTime / It.Value.Count * 1000,
					
					V.ValuesTime,
					V.ValuesTime / V.TotalTime * 100,
					V.ValuesAccesses > 0 ? V.ValuesTime / V.ValuesAccesses * 1e9 : 0,
					
					V.MaterialsTime,
					V.MaterialsTime / V.TotalTime * 100,
					V.MaterialsAccesses > 0 ? V.MaterialsTime / V.MaterialsAccesses * 1e9 : 0,
					
					V.NormalsTime,
					V.NormalsTime / V.TotalTime * 100,
					
					V.UVsTime,
					V.UVsTime / V.TotalTime * 100,
					
					V.CreateChunkTime,
					V.CreateChunkTime / V.TotalTime * 100);
			}

			return GlobalTotalTime;
		};
		
		auto& LocalStats = Singleton.StatsMap.FindOrAdd(World);
		UE_LOG(LogVoxel, Log, TEXT("###############################################################################"));
		UE_LOG(LogVoxel, Log, TEXT("################################# Voxel Stats #################################"));
		UE_LOG(LogVoxel, Log, TEXT("###############################################################################"));
		UE_LOG(LogVoxel, Log, TEXT("World: %s"), *World->GetName());
		UE_LOG(LogVoxel, Log, TEXT("Normal Chunks:"));
		const double NormalTime = Print(LocalStats.NormalStats);
		UE_LOG(LogVoxel, Log, TEXT("Transitions Chunks:"));
		const double TransitionsTime = Print(LocalStats.TransitionsStats);
		UE_LOG(LogVoxel, Log, TEXT("Geometry Chunks (Spawners):"));
		const double GeometryTime = Print(LocalStats.GeometryStats);
		UE_LOG(LogVoxel, Log, TEXT("###############################################################################"));
		UE_LOG(LogVoxel, Log, TEXT("################################### Summary ###################################"));
		UE_LOG(LogVoxel, Log, TEXT("###############################################################################"));
		UE_LOG(LogVoxel, Log, TEXT("Main Time: %fs"), NormalTime);
		UE_LOG(LogVoxel, Log, TEXT("Transitions Time: %fs"), TransitionsTime);
		UE_LOG(LogVoxel, Log, TEXT("Geometry Time: %fs"), GeometryTime);
		UE_LOG(LogVoxel, Log, TEXT("Total Time: %fs"), NormalTime + TransitionsTime + GeometryTime);
		UE_LOG(LogVoxel, Log, TEXT("Values Time: %3.2f%% of total time"), 100 * TotalValuesTime / (NormalTime + TransitionsTime + GeometryTime));
		UE_LOG(LogVoxel, Log, TEXT("Transitions Time: %3.2f%% of Main + Transitions"), 100 * TransitionsTime / (NormalTime + TransitionsTime));
		UE_LOG(LogVoxel, Log, TEXT("Values: %llu reads in %fs, avg %.1fns/voxel"), TotalValuesAccesses, TotalValuesTime, TotalValuesTime / TotalValuesAccesses * 1e9);
		UE_LOG(LogVoxel, Log, TEXT("Materials: %llu reads in %fs, avg %.1fns/voxel"), TotalMaterialsAccesses, TotalMaterialsTime, TotalMaterialsTime / TotalMaterialsAccesses * 1e9);
	}
};

static FAutoConsoleCommandWithWorld ClearMesherStatsCmd(
	TEXT("voxel.mesher.ClearStats"),
	TEXT("Clear the mesher stats"),
	FConsoleCommandWithWorldDelegate::CreateStatic(&FVoxelMesherStats::Clear));

static FAutoConsoleCommandWithWorld PrintMesherStatsCmd(
	TEXT("voxel.mesher.PrintStats"),
	TEXT("Print the mesher stats"),
	FConsoleCommandWithWorldDelegate::CreateStatic(&FVoxelMesherStats::PrintStats));

FVoxelMesherStats FVoxelMesherStats::Singleton;

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

FVoxelMesherBase::FVoxelMesherBase(
	int32 LOD,
	const FIntVector& ChunkPosition,
	const FVoxelRendererSettings& Settings,
	bool bIsTransitions)
	: LOD(LOD)
	, Step(1 << LOD)
	, Size(RENDER_CHUNK_SIZE << LOD)
	, ChunkPosition(ChunkPosition)
	, Settings(Settings)
	, Data(*Settings.Data)
	, bIsTransitions(bIsTransitions)
{
}

FVoxelMesherBase::~FVoxelMesherBase()
{
}

void FVoxelMesherBase::UnlockData()
{
	Data.Unlock(MoveTemp(LockInfo));
}

void FVoxelMesherBase::LockData()
{
	LockInfo = Data.Lock(EVoxelLockType::Read, GetBoundsToLock(), "Mesher");
}

bool FVoxelMesherBase::IsEmpty() const
{
	const FIntBox Bounds = GetBoundsToCheckIsEmptyOn();
	const bool bIsEmpty = CVarDoNotSkipEmptyChunks.GetValueOnAnyThread() != 0 ? false : Data.IsEmpty(Bounds, LOD);

	if (!bIsTransitions)
	{
		VOXEL_SCOPE_COUNTER("DebugIsEmpty");
		const FIntBox BoundsCopy(Bounds.Min, Bounds.Max - FIntVector(Step));
		AsyncTask(ENamedThreads::GameThread, [WeakDebug = MakeVoxelWeakPtr(Settings.DebugManager), BoundsCopy, bIsEmpty]
			{
				auto Debug = WeakDebug.Pin();
				if (Debug.IsValid())
				{
					Debug->ReportChunkEmptyState(BoundsCopy, bIsEmpty);
				}
			});
	}
	
	return bIsEmpty;
}

TVoxelSharedPtr<FVoxelChunkMesh> FVoxelMesherBase::CreateEmptyChunk() const
{
	auto Chunk = MakeVoxelShared<FVoxelChunkMesh>();
	if (Settings.MaterialConfig == EVoxelMaterialConfig::RGB)
	{
		Chunk->SetIsSingle(true);
		Chunk->CreateSingleBuffers();
	}
	return Chunk;
}

void FVoxelMesherBase::FinishCreatingChunk(FVoxelChunkMesh& Chunk) const
{
	if (Settings.bOptimizeIndices)
	{
		Chunk.IterateBuffers([](auto& Buffer) { Buffer.OptimizeIndices(); });
	}
	Chunk.IterateBuffers([](auto& Buffer) { Buffer.Shrink(); });

	Chunk.ComputeBounds();
	Chunk.ComputeGuid();
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

FVoxelMesher::FVoxelMesher(
	int32 LOD,
	const FIntVector& ChunkPosition,
	const FVoxelRendererSettings& Settings)
	: FVoxelMesherBase(LOD, ChunkPosition, Settings, false)
{
}

TVoxelSharedPtr<FVoxelChunkMesh> FVoxelMesher::CreateFullChunk()
{
	VOXEL_SCOPE_COUNTER_FORMAT("Creating Chunk LOD=%d", LOD);

	{
		VOXEL_SCOPE_COUNTER("InitArea");
		Data.WorldGenerator->InitArea(FIntBox(ChunkPosition, ChunkPosition + Step * RENDER_CHUNK_SIZE), LOD);
	}

	LockData();

	TVoxelSharedPtr<FVoxelChunkMesh> Chunk;
	if (IsEmpty())
	{
		Chunk = CreateEmptyChunk();
		UnlockData();
	}
	else
	{
		const double StartTime = FPlatformTime::Seconds();
		FVoxelMesherTimes Times;
		Chunk = CreateFullChunkImpl(Times);
		check(!LockInfo.IsValid());
		const double EndTime = FPlatformTime::Seconds();
		FVoxelMesherStats::Report(Settings.World, LOD, EndTime - StartTime, Times, false, false);
	}

	if (Chunk.IsValid())
	{
		FinishCreatingChunk(*Chunk);

		if (LOD <= Settings.MaxDistanceFieldLOD)
		{
			Chunk->BuildDistanceField(LOD, ChunkPosition, Data);
		}
	}
	
	return Chunk;
}

void FVoxelMesher::CreateGeometry(TArray<uint32>& Indices, TArray<FVector>& Vertices)
{
	VOXEL_SCOPE_COUNTER_FORMAT("Creating Geometry LOD=%d", LOD);

	{
		VOXEL_SCOPE_COUNTER("InitArea");
		Data.WorldGenerator->InitArea(FIntBox(ChunkPosition, ChunkPosition + Step * RENDER_CHUNK_SIZE), LOD);
	}

	LockData();

	if (IsEmpty())
	{
		UnlockData();
	}
	else
	{
		const double StartTime = FPlatformTime::Seconds();
		FVoxelMesherTimes Times;
		CreateGeometryImpl(Times, Indices, Vertices);
		check(!LockInfo.IsValid());
		const double EndTime = FPlatformTime::Seconds();
		FVoxelMesherStats::Report(Settings.World, LOD, EndTime - StartTime, Times, false, true);
	}
}

///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////

FVoxelTransitionsMesher::FVoxelTransitionsMesher(
	int32 LOD,
	const FIntVector& ChunkPosition,
	const FVoxelRendererSettings& Settings,
	uint8 TransitionsMask)
	: FVoxelMesherBase(LOD, ChunkPosition, Settings, true)
	, TransitionsMask(TransitionsMask)
	, HalfLOD(LOD - 1)
	, HalfStep(Step / 2)
{
}

TVoxelSharedPtr<FVoxelChunkMesh> FVoxelTransitionsMesher::CreateFullChunk()
{
	VOXEL_SCOPE_COUNTER_FORMAT("Creating Transitions LOD=%d Num=%u", LOD, FVoxelUtilities::Popc(TransitionsMask));

	check(TransitionsMask);

	LockData();
	
	TVoxelSharedPtr<FVoxelChunkMesh> Chunk;
	if (IsEmpty())
	{
		Chunk = CreateEmptyChunk();
		UnlockData();
	}
	else
	{
		const double StartTime = FPlatformTime::Seconds();
		FVoxelMesherTimes Times;
		Chunk = CreateFullChunkImpl(Times);
		check(!LockInfo.IsValid());
		const double EndTime = FPlatformTime::Seconds();
		FVoxelMesherStats::Report(Settings.World, LOD, EndTime - StartTime, Times, true, false);
	}

	if (Chunk.IsValid())
	{
		FinishCreatingChunk(*Chunk);
	}

	return Chunk;
}

void FVoxelTransitionsMesher::CreateGeometry(TArray<uint32>& Indices, TArray<FVector>& Vertices)
{
	unimplemented();
}