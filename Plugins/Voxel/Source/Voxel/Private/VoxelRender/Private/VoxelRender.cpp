// Copyright 2017 Phyronnaz

#include "VoxelPrivatePCH.h"
#include "VoxelRender.h"
#include "VoxelChunkComponent.h"
#include "ChunkOctree.h"

DECLARE_CYCLE_STAT(TEXT("VoxelRender ~ ApplyUpdates"), STAT_ApplyUpdates, STATGROUP_Voxel);
DECLARE_CYCLE_STAT(TEXT("VoxelRender ~ UpdateLOD"), STAT_UpdateLOD, STATGROUP_Voxel);

FVoxelRender::FVoxelRender(AVoxelWorld* World, AActor* ChunksParent, FVoxelData* Data, uint32 MeshThreadCount, uint32 FoliageThreadCount)
	: World(World)
	, ChunksParent(ChunksParent)
	, Data(Data)
	, MeshThreadPool(FQueuedThreadPool::Allocate())
	, FoliageThreadPool(FQueuedThreadPool::Allocate())
	, TimeSinceFoliageUpdate(0)
{
	// Add existing chunks
	for (auto Component : ChunksParent->GetComponentsByClass(UVoxelChunkComponent::StaticClass()))
	{
		UVoxelChunkComponent* ChunkComponent = Cast<UVoxelChunkComponent>(Component);
		if (ChunkComponent)
		{
			ChunkComponent->Delete();
			ChunkComponent->SetVoxelMaterial(World->GetVoxelMaterial());
			InactiveChunks.push_front(ChunkComponent);
		}
	}
	// Delete existing grass components
	for (auto Component : ChunksParent->GetComponentsByClass(UHierarchicalInstancedStaticMeshComponent::StaticClass()))
	{
		Component->DestroyComponent();
	}

	MeshThreadPool->Create(MeshThreadCount, 64 * 1024);
	FoliageThreadPool->Create(FoliageThreadCount, 32 * 1024);

	MainOctree = MakeShareable(new FChunkOctree(this, FIntVector::ZeroValue, Data->Depth, FOctree::GetTopIdFromDepth(Data->Depth)));
}

FVoxelRender::~FVoxelRender()
{
	MeshThreadPool->Destroy();
	FoliageThreadPool->Destroy();
}

void FVoxelRender::Tick(float DeltaTime)
{
	TimeSinceFoliageUpdate += DeltaTime;

	UpdateLOD();
	ApplyUpdates();

	if (TimeSinceFoliageUpdate > 1 / World->GetFoliageFPS())
	{
		ApplyFoliageUpdates();
		TimeSinceFoliageUpdate = 0;
	}

	ApplyNewMeshes();
	ApplyNewFoliages();
}

void FVoxelRender::AddInvoker(TWeakObjectPtr<UVoxelInvokerComponent> Invoker)
{
	VoxelInvokerComponents.push_front(Invoker);
}

UVoxelChunkComponent* FVoxelRender::GetInactiveChunk()
{
	UVoxelChunkComponent* Chunk;
	if (InactiveChunks.empty())
	{
		Chunk = NewObject<UVoxelChunkComponent>(ChunksParent);;
		Chunk->OnComponentCreated();
		Chunk->RegisterComponent();
		Chunk->SetVoxelMaterial(World->GetVoxelMaterial());
	}
	else
	{
		Chunk = InactiveChunks.front();
		InactiveChunks.pop_front();
	}
	ActiveChunks.Add(Chunk);

	check(Chunk->IsValidLowLevel());
	return Chunk;
}

void FVoxelRender::SetChunkAsInactive(UVoxelChunkComponent* Chunk)
{
	ActiveChunks.Remove(Chunk);
	InactiveChunks.push_front(Chunk);

	RemoveFromQueues(Chunk);
}

void FVoxelRender::UpdateChunk(TWeakPtr<FChunkOctree> Chunk, bool bAsync)
{
	if (Chunk.IsValid())
	{
		ChunksToUpdate.Add(Chunk);
		if (!bAsync)
		{
			IdsOfChunksToUpdateSynchronously.Add(Chunk.Pin().Get()->Id);
		}
	}
}

void FVoxelRender::UpdateChunksAtPosition(FIntVector Position, bool bAsync)
{
	check(Data->IsInWorld(Position.X, Position.Y, Position.Z));

	int X = Position.X + Data->Size() / 2;
	int Y = Position.Y + Data->Size() / 2;
	int Z = Position.Z + Data->Size() / 2;

	bool bXIsAtBorder = (X % 16 == 0) && (X != 0);
	bool bYIsAtBorder = (Y % 16 == 0) && (Y != 0);
	bool bZIsAtBorder = (Z % 16 == 0) && (Z != 0);

	UpdateChunk(MainOctree->GetLeaf(Position), bAsync);

	if (bXIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(8, 0, 0)), bAsync);
	}
	if (bYIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(0, 8, 0)), bAsync);
	}
	if (bXIsAtBorder && bYIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(8, 8, 0)), bAsync);
	}
	if (bZIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(0, 0, 8)), bAsync);
	}
	if (bXIsAtBorder && bZIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(8, 0, 8)), bAsync);
	}
	if (bYIsAtBorder && bZIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(0, 8, 8)), bAsync);
	}
	if (bXIsAtBorder && bYIsAtBorder && bZIsAtBorder)
	{
		UpdateChunk(MainOctree->GetLeaf(Position - FIntVector(8, 8, 8)), bAsync);
	}
}

void FVoxelRender::ApplyUpdates()
{
	SCOPE_CYCLE_COUNTER(STAT_ApplyUpdates);

	std::forward_list<TWeakPtr<FChunkOctree>> Failed;

	for (auto& Chunk : ChunksToUpdate)
	{
		TSharedPtr<FChunkOctree> LockedChunk(Chunk.Pin());

		if (LockedChunk.IsValid() && LockedChunk->GetVoxelChunk())
		{
			bool bAsync = !IdsOfChunksToUpdateSynchronously.Contains(LockedChunk->Id);
			bool bSuccess = LockedChunk->GetVoxelChunk()->Update(bAsync);

			/*if (!bSuccess)
			{
				UE_LOG(VoxelLog, Warning, TEXT("Chunk already updating"));
			}*/
		}
	}
	ChunksToUpdate.Reset();
	IdsOfChunksToUpdateSynchronously.Reset();

	// See Init and Unload functions of AVoxelChunk
	ApplyTransitionChecks();
}

void FVoxelRender::UpdateAll(bool bAsync)
{
	for (auto Chunk : ActiveChunks)
	{
		Chunk->Update(bAsync);
	}
}

void FVoxelRender::UpdateLOD()
{
	SCOPE_CYCLE_COUNTER(STAT_UpdateLOD);

	// Clean
	std::forward_list<TWeakObjectPtr<UVoxelInvokerComponent>> Temp;
	for (auto Invoker : VoxelInvokerComponents)
	{
		if (Invoker.IsValid())
		{
			Temp.push_front(Invoker);
		}
	}
	VoxelInvokerComponents = Temp;

	MainOctree->UpdateLOD(VoxelInvokerComponents);
}

void FVoxelRender::AddFoliageUpdate(UVoxelChunkComponent* Chunk)
{
	FoliageUpdateNeeded.Add(Chunk);
}

void FVoxelRender::AddTransitionCheck(UVoxelChunkComponent* Chunk)
{
	ChunksToCheckForTransitionChange.Add(Chunk);
}

void FVoxelRender::AddApplyNewMesh(UVoxelChunkComponent* Chunk)
{
	FScopeLock Lock(&ChunksToApplyNewMeshLock);
	ChunksToApplyNewMesh.Add(Chunk);
}

void FVoxelRender::AddApplyNewFoliage(UVoxelChunkComponent* Chunk)
{
	FScopeLock Lock(&ChunksToApplyNewFoliageLock);
	ChunksToApplyNewFoliage.Add(Chunk);
}

void FVoxelRender::RemoveFromQueues(UVoxelChunkComponent* Chunk)
{
	FoliageUpdateNeeded.Remove(Chunk);
	{
		FScopeLock Lock(&ChunksToApplyNewMeshLock);
		ChunksToApplyNewMesh.Remove(Chunk);
	}
	{
		FScopeLock Lock(&ChunksToApplyNewFoliageLock);
		ChunksToApplyNewFoliage.Remove(Chunk);
	}
}

void FVoxelRender::ApplyFoliageUpdates()
{
	std::forward_list<UVoxelChunkComponent*> Failed;
	for (auto Chunk : FoliageUpdateNeeded)
	{
		bool bSuccess = Chunk->UpdateFoliage();
		if (!bSuccess)
		{
			Failed.push_front(Chunk);
		}
	}
	FoliageUpdateNeeded.Empty();

	for (auto Chunk : Failed)
	{
		FoliageUpdateNeeded.Add(Chunk);
	}
}

void FVoxelRender::ApplyTransitionChecks()
{
	for (auto Chunk : ChunksToCheckForTransitionChange)
	{
		Chunk->CheckTransitions();
	}
	ChunksToCheckForTransitionChange.Empty();
}

void FVoxelRender::ApplyNewMeshes()
{
	FScopeLock Lock(&ChunksToApplyNewMeshLock);
	for (auto Chunk : ChunksToApplyNewMesh)
	{
		Chunk->ApplyNewMesh();
	}
	ChunksToApplyNewMesh.Empty();
}

void FVoxelRender::ApplyNewFoliages()
{
	FScopeLock Lock(&ChunksToApplyNewFoliageLock);
	for (auto Chunk : ChunksToApplyNewFoliage)
	{
		Chunk->ApplyNewFoliage();
	}
	ChunksToApplyNewFoliage.Empty();
}

TWeakPtr<FChunkOctree> FVoxelRender::GetChunkOctreeAt(FIntVector Position) const
{
	check(Data->IsInWorld(Position.X, Position.Y, Position.Z));
	return MainOctree->GetLeaf(Position);
}

int FVoxelRender::GetDepthAt(FIntVector Position) const
{
	return GetChunkOctreeAt(Position).Pin()->Depth;
}

void FVoxelRender::Delete()
{
	MeshThreadPool->Destroy();
	FoliageThreadPool->Destroy();

	for (auto Chunk : ActiveChunks)
	{
		Chunk->Delete();
		InactiveChunks.push_front(Chunk);
	}

	for (auto Chunk : InactiveChunks)
	{
		Chunk->DestroyComponent();
	}
}

FVector FVoxelRender::GetGlobalPosition(FIntVector LocalPosition)
{
	return World->LocalToGlobal(LocalPosition) + ChunksParent->GetActorLocation() - World->GetActorLocation();
}
