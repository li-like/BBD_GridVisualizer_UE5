#include "Render/GeoSOTISMManager.h"


#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// 构造函数
AGeoSOTISMManager::AGeoSOTISMManager()
{
    PrimaryActorTick.bCanEverTick = true;
    bIsUpdatingChunks = false;
    CurrentRecheckTimer = 0.0f;
    LastUpdateLocation = FVector(0, 0, -99999999); // 确保第一次一定会更新

    // 默认加载一个立方体 Mesh，防止 BaseMesh 为空
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeAsset(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (CubeAsset.Succeeded())
    {
        BaseMesh = CubeAsset.Object;
    }
}

void AGeoSOTISMManager::BeginPlay()
{
    Super::BeginPlay();

    // 1. 自动获取 Georeference
    if (!Georeference)
    {
        Georeference = Cast<ACesiumGeoreference>(
            UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass()));
    }

    // 2. 自动获取 Cesium3DTileset (用于监控或碰撞忽略)
    if (!TargetTileset)
    {
        TargetTileset = Cast<ACesium3DTileset>(
            UGameplayStatics::GetActorOfClass(GetWorld(), ACesium3DTileset::StaticClass()));
    }

    // 3. 初始延迟更新，给 Cesium 一点初始化的时间
    FTimerHandle InitTimer;
    GetWorld()->GetTimerManager().SetTimer(InitTimer, [this]()
    {
        APawn* Player = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
        if (Player)
        {
            UpdateChunks(Player->GetActorLocation());
        }
    }, 0.5f, false);
}

void AGeoSOTISMManager::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!Georeference) return;

    APawn* Player = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!Player) return;

    FVector CurrentLoc = Player->GetActorLocation();

    // ---------------------------------------------------------
    // 任务 A: 空间移动更新 (加载/卸载大块 Chunk)
    // ---------------------------------------------------------
    if (FVector::Dist(CurrentLoc, LastUpdateLocation) > 10000.0f) // 每移动 100米 检查一次 Chunk 边界
    {
        UpdateChunks(CurrentLoc);
        LastUpdateLocation = CurrentLoc;
    }

    // ---------------------------------------------------------
    // 任务 B: 实时重检 (动态发现新加载的建筑物)
    // ---------------------------------------------------------
    CurrentRecheckTimer += DeltaTime;
    if (CurrentRecheckTimer >= RecheckInterval)
    {
        ProcessRealTimeDetection();
        CurrentRecheckTimer = 0.0f;
    }
}

// =================================================================
// 核心逻辑：空间管理
// =================================================================

void AGeoSOTISMManager::UpdateChunks(const FVector& PlayerLocation)
{
    if (bIsUpdatingChunks || !Georeference) return;
    bIsUpdatingChunks = true;

    // 1. 获取经纬度
    FVector PlayerLLH = Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(PlayerLocation);

    // 2. 计算视野范围 (基于 ChunkLevel)
    double DegRadius = ViewRadius / UGeoSOTUtils::METERS_PER_DEGREE;
    FVector2D StartLL(PlayerLLH.X - DegRadius, PlayerLLH.Y - DegRadius);
    FVector2D EndLL(PlayerLLH.X + DegRadius, PlayerLLH.Y + DegRadius);

    // 3. 计算需要的 Chunk 列表
    // 高度范围给个宽泛的即可，Chunk 主要是为了 2D 分块管理
    TArray<FGeoSOTCell> NeededChunks = UGeoSOTUtils::GenerateGridsByRange3D(
        StartLL, EndLL, 0, 5000, ChunkLevel);

    TSet<int64> NewKeys;
    for (const FGeoSOTCell& Cell : NeededChunks)
    {
        int64 Key = ((int64)Cell.Code.Row << 32) | (int64)Cell.Code.Col;
        NewKeys.Add(Key);

        // 如果这个块还没创建，就创建它
        if (!ChunkMap.Contains(Key))
        {
            CreateChunk(Key, Cell);
        }
    }

    // 4. 清理超出视野的 Chunk
    TArray<int64> KeysToRemove;
    for (auto& Elem : ChunkMap)
    {
        if (!NewKeys.Contains(Elem.Key))
        {
            // 销毁组件
            if (Elem.Value.FreeISM) Elem.Value.FreeISM->DestroyComponent();
            if (Elem.Value.OccupiedISM) Elem.Value.OccupiedISM->DestroyComponent();
            KeysToRemove.Add(Elem.Key);
        }
    }

    for (int64 Key : KeysToRemove)
    {
        ChunkMap.Remove(Key);
    }

    bIsUpdatingChunks = false;
}

// =================================================================
// 核心逻辑：生成与渲染
// =================================================================

void AGeoSOTISMManager::CreateChunk(int64 ChunkKey, const FGeoSOTCell& ChunkCell)
{
    if (!BaseMesh || !GridMaterial || !OccupiedMaterial) return;

    // 参数捕获
    double MinH = MinAltitude;
    double MaxH = LowAltitudeCeiling;
    int32 TargetLevel = RenderLevel;
    float Thickness = LineThicknessCm;

    // -------------------------------------------------------------
    // Phase 1: 异步计算 (在后台线程计算成千上万个网格的经纬度)
    // -------------------------------------------------------------
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, ChunkKey, ChunkCell, MinH, MaxH, TargetLevel, Thickness]()
    {
        // 计算该 Chunk 下所有的细分网格
        TArray<FGeoSOTCell> SubGrids = UGeoSOTUtils::GenerateGridsByRange3D(
            FVector2D(ChunkCell.MinLon, ChunkCell.MinLat),
            FVector2D(ChunkCell.MaxLon, ChunkCell.MaxLat),
            MinH, MaxH, TargetLevel);

        // ---------------------------------------------------------
        // Phase 2: 回到主线程进行渲染和碰撞检测
        // ---------------------------------------------------------
        AsyncTask(ENamedThreads::GameThread, [this, ChunkKey, SubGrids, Thickness]()
        {
            // 安全检查：如果 Actor 已经被销毁或 Key 已经被移除了，就停止
            if (!IsValid(this)) return;

            // 1. 初始化 Chunk 数据包
            FChunkPackage NewPackage;

            // 2. 创建 ISM 组件
            auto CreateISM = [&](UMaterialInterface* Mat, FName Name) -> UInstancedStaticMeshComponent*
            {
                UInstancedStaticMeshComponent* Comp = NewObject<UInstancedStaticMeshComponent>(this, Name);
                Comp->RegisterComponent();
                Comp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
                Comp->SetStaticMesh(BaseMesh);
                Comp->SetMaterial(0, Mat);
                Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision); // 视觉组件无碰撞
                Comp->SetCastShadow(false);
                return Comp;
            };

            NewPackage.FreeISM = CreateISM(GridMaterial, FName(*FString::Printf(TEXT("Free_%lld"), ChunkKey)));
            NewPackage.OccupiedISM = CreateISM(OccupiedMaterial, FName(*FString::Printf(TEXT("Occ_%lld"), ChunkKey)));

            // 3. 批量处理变换数据
            TArray<FTransform> BatchFreeTransforms;
            TArray<FTransform> BatchOccupiedTransforms;
            
            // 预留内存优化性能
            BatchFreeTransforms.Reserve(SubGrids.Num() * 12); 
            NewPackage.SuspectedFreeGrids.Reserve(SubGrids.Num());

            // 4. 遍历每个细分网格
            for (const FGeoSOTCell& Grid : SubGrids)
            {
                // 计算中心点和包围盒
                double CenterLon = (Grid.MinLon + Grid.MaxLon) * 0.5;
                double CenterLat = (Grid.MinLat + Grid.MaxLat) * 0.5;
                double CenterAlt = (Grid.MinHeight + Grid.MaxHeight) * 0.5;

                FVector CenterPos = LlhToUnreal(CenterLon, CenterLat, CenterAlt);
                FVector CornerPos = LlhToUnreal(Grid.MinLon, Grid.MinLat, Grid.MinHeight);
                FVector Extent = (CenterPos - CornerPos).GetAbs() * 0.95f; // 略微缩小避免边缘误触

                // 立即进行一次物理检测
                bool bHit = CheckGridCollision(CenterPos, Extent);

                // 计算 12 条边的 Transform
                TArray<FTransform> WireTransforms;
                GetWireframeTransforms(CenterPos, Extent, Thickness, WireTransforms);

                if (bHit)
                {
                    // 障碍物 -> 直接加到红色组件
                    BatchOccupiedTransforms.Append(WireTransforms);
                }
                else
                {
                    // 空闲 -> 加到绿色组件
                    // [关键] 记录起始索引，以便后续修改。当前 FreeISM 的数量 + 已经放入 Batch 的数量
                    int32 StartIdx = NewPackage.FreeISM->GetInstanceCount() + BatchFreeTransforms.Num();
                    
                    BatchFreeTransforms.Append(WireTransforms);

                    // 加入待重检列表
                    FSimpleGridInfo Info;
                    Info.Center = CenterPos;
                    Info.Extent = Extent;
                    Info.InstanceStartIndex = StartIdx; // 这里存的是这12根线的起始索引
                    NewPackage.SuspectedFreeGrids.Add(Info);
                }
            }

            // 5. 提交 ISM 实例
            if (BatchFreeTransforms.Num() > 0) NewPackage.FreeISM->AddInstances(BatchFreeTransforms, false);
            if (BatchOccupiedTransforms.Num() > 0) NewPackage.OccupiedISM->AddInstances(BatchOccupiedTransforms, false);

            // 6. 注册到 Map
            ChunkMap.Add(ChunkKey, NewPackage);
        });
    });
}

// =================================================================
// 核心逻辑：实时检测与状态修正
// =================================================================

void AGeoSOTISMManager::ProcessRealTimeDetection()
{
    if (ChunkMap.Num() == 0) return;

    // 遍历每一个 Chunk
    for (auto& Pair : ChunkMap)
    {
        FChunkPackage& Package = Pair.Value;
        
        // 如果没有待检测的网格，直接跳过
        if (Package.SuspectedFreeGrids.Num() == 0) continue;

        TArray<int32> IndicesToRemove; // 记录需要从 Suspected 列表中移除的下标

        // 倒序遍历（方便删除，虽然后面用 RemoveAllSwap 也可以）
        for (int32 i = Package.SuspectedFreeGrids.Num() - 1; i >= 0; --i)
        {
            const FSimpleGridInfo& Info = Package.SuspectedFreeGrids[i];

            // 再次检测物理碰撞
            if (CheckGridCollision(Info.Center, Info.Extent))
            {
                // === 发现新障碍物！状态翻转：绿 -> 红 ===

                // 1. 隐藏绿色线框 (Scale设为0)
                // 解释：ISM 移除实例会导致后续索引前移，管理极其复杂。
                // 最高效的方法是将原来的实例缩放为 0，让它看不见。
                FTransform ZeroTransform;
                ZeroTransform.SetScale3D(FVector::ZeroVector); // 设为 0

                // 线框有 12 条边，我们需要把这 12 个 Instance 全都隐藏
                for (int32 EdgeIdx = 0; EdgeIdx < 12; ++EdgeIdx)
                {
                    // 注意：UpdateInstanceTransform 最后一个参数 bMarkRenderStateDirty 设为 false
                    // 我们在循环结束后手动 Mark 一次，性能更好
                    Package.FreeISM->UpdateInstanceTransform(Info.InstanceStartIndex + EdgeIdx, ZeroTransform, true, false);
                }

                // 2. 在红色组件中新增线框
                TArray<FTransform> NewRedTransforms;
                GetWireframeTransforms(Info.Center, Info.Extent, LineThicknessCm, NewRedTransforms);
                Package.OccupiedISM->AddInstances(NewRedTransforms, false, false); // 同样暂不刷新渲染

                // 3. 标记该 Info 需要被移除 (因为它已经是红色的了，不需要再检测)
                IndicesToRemove.Add(i);
            }
        }

        // 批量移除已变红的网格数据
        if (IndicesToRemove.Num() > 0)
        {
            for (int32 Index : IndicesToRemove)
            {
                Package.SuspectedFreeGrids.RemoveAtSwap(Index); // Swap 比 RemoveAt 快，因为顺序不重要
            }

            // 统一刷新渲染状态
            Package.FreeISM->MarkRenderStateDirty();
            Package.OccupiedISM->MarkRenderStateDirty();
        }
    }
}

// =================================================================
// 工具函数
// =================================================================

bool AGeoSOTISMManager::CheckGridCollision(const FVector& Center, const FVector& Extent) const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    FCollisionQueryParams Params;
    Params.bTraceComplex = true; // 【关键】Cesium 3D Tiles 必须开启复杂检测
    Params.AddIgnoredActor(this);
    
    APawn* Player = UGameplayStatics::GetPlayerPawn(World, 0);
    if (Player) Params.AddIgnoredActor(Player);

    // 检测 WorldStatic (建筑/地形) 和 WorldDynamic (可能有的动态障碍)
    // OverlapBlockingTestByChannel 比 Sweep 快一点，适合纯粹的"有没有东西"判断
    return World->OverlapBlockingTestByChannel(
        Center,
        FQuat::Identity,
        ECC_WorldStatic, 
        FCollisionShape::MakeBox(Extent*1.5f),
        Params
    );
}

FVector AGeoSOTISMManager::LlhToUnreal(double Lon, double Lat, double Alt)
{
    if (Georeference)
    {
        return Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Lon, Lat, Alt));
    }
    return FVector::ZeroVector;
}

void AGeoSOTISMManager::GetWireframeTransforms(const FVector& Center, const FVector& Extent, float Thickness, TArray<FTransform>& OutTransforms)
{
    // 计算 8 个顶点
    FVector V[8];
    FVector MinP = Center - Extent;
    FVector MaxP = Center + Extent;

    V[0] = FVector(MinP.X, MinP.Y, MinP.Z); 
    V[1] = FVector(MaxP.X, MinP.Y, MinP.Z); 
    V[2] = FVector(MaxP.X, MaxP.Y, MinP.Z); 
    V[3] = FVector(MinP.X, MaxP.Y, MinP.Z); 
    V[4] = FVector(MinP.X, MinP.Y, MaxP.Z); 
    V[5] = FVector(MaxP.X, MinP.Y, MaxP.Z); 
    V[6] = FVector(MaxP.X, MaxP.Y, MaxP.Z); 
    V[7] = FVector(MinP.X, MaxP.Y, MaxP.Z); 

    auto AddEdge = [&](const FVector& Start, const FVector& End)
    {
        FVector Mid = (Start + End) * 0.5f;
        FVector Dir = End - Start;
        float Len = Dir.Size();
        
        if (Len > 0.1f)
        {
            FQuat Rot = FRotationMatrix::MakeFromX(Dir).ToQuat();
            // 假设 BaseMesh 是 100cm 的 Cube
            FVector Scale(Len / 100.0f, Thickness / 100.0f, Thickness / 100.0f);
            OutTransforms.Add(FTransform(Rot, Mid, Scale));
        }
    };

    // 底部 4 边
    AddEdge(V[0], V[1]); AddEdge(V[1], V[2]); AddEdge(V[2], V[3]); AddEdge(V[3], V[0]);
    // 顶部 4 边
    AddEdge(V[4], V[5]); AddEdge(V[5], V[6]); AddEdge(V[6], V[7]); AddEdge(V[7], V[4]);
    // 垂直 4 边
    AddEdge(V[0], V[4]); AddEdge(V[1], V[5]); AddEdge(V[2], V[6]); AddEdge(V[3], V[7]);
}