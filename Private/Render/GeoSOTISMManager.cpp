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

    // 安全检查
    if (!Georeference) return;
    APawn* Player = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
    if (!Player) return;

    FVector CurrentLoc = Player->GetActorLocation();

    // ---------------------------------------------------------
    // 任务 A: 空间移动更新 (加载/卸载大块 Chunk)
    // ---------------------------------------------------------
    // 逻辑保持不变：每移动 100米 检查一次是否需要加载新区块
    if (FVector::Dist(CurrentLoc, LastUpdateLocation) > 10000.0f) 
    {
        UpdateChunks(CurrentLoc);
        LastUpdateLocation = CurrentLoc;
    }

    // ---------------------------------------------------------
    // 任务 B: 实时重检 (逻辑升级：近快远慢)
    // ---------------------------------------------------------
    
    // 累加时间
    NearTimer += DeltaTime;
    FarTimer += DeltaTime;

    // 1. 检查是否到了"高频检测"的时间点 (例如 0.1s)
    if (NearTimer >= NearCheckInterval)
    {
        // 重置近处计时器
        NearTimer = 0.0f;

        // 2. 检查是否同时也到了"低频检测"的时间点 (例如 1.0s)
        bool bCheckFar = false;
        if (FarTimer >= FarCheckInterval)
        {
            FarTimer = 0.0f;
            bCheckFar = true; // 标记：这一帧也要检查远处
        }

        // 执行检测
        // 参数含义：现在玩家在哪里？这一帧是否允许检查远处的网格？
        ProcessRealTimeDetection(CurrentLoc, bCheckFar);
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
    if (!BaseMesh || !SafeWireMaterial || !ObstacleWireMaterial) return;

    // 参数捕获
    double MinH = MinAltitude;
    double MaxH = LowAltitudeCeiling;
    int32 TargetLevel = RenderLevel;


    // -------------------------------------------------------------
    // Phase 1: 异步计算 (在后台线程计算成千上万个网格的经纬度)
    // -------------------------------------------------------------
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, ChunkKey, ChunkCell, MinH, MaxH, TargetLevel]()
    {
        // 计算该 Chunk 下所有的细分网格
        TArray<FGeoSOTCell> SubGrids = UGeoSOTUtils::GenerateGridsByRange3D(
            FVector2D(ChunkCell.MinLon, ChunkCell.MinLat),
            FVector2D(ChunkCell.MaxLon, ChunkCell.MaxLat),
            MinH, MaxH, TargetLevel);

        // ---------------------------------------------------------
        // Phase 2: 回到主线程进行渲染和碰撞检测
        // ---------------------------------------------------------
        AsyncTask(ENamedThreads::GameThread, [this, ChunkKey, SubGrids]()
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

            // 使用你在蓝图中指定的 材质实例
            NewPackage.FreeISM = CreateISM(SafeWireMaterial, FName(*FString::Printf(TEXT("Free_%lld"), ChunkKey)));
            NewPackage.OccupiedISM = CreateISM(ObstacleWireMaterial, FName(*FString::Printf(TEXT("Occ_%lld"), ChunkKey)));

            TArray<FTransform> BatchFreeTransforms;
            TArray<FTransform> BatchOccupiedTransforms;

            BatchFreeTransforms.Reserve(SubGrids.Num()); 
            NewPackage.SuspectedFreeGrids.Reserve(SubGrids.Num());

            // 3. 遍历每个细分网格
            for (const FGeoSOTCell& Grid : SubGrids)
            {
                // 计算中心点和包围盒
                double CenterLon = (Grid.MinLon + Grid.MaxLon) * 0.5;
                double CenterLat = (Grid.MinLat + Grid.MaxLat) * 0.5;
                double CenterAlt = (Grid.MinHeight + Grid.MaxHeight) * 0.5;

                FVector CenterPos = LlhToUnreal(CenterLon, CenterLat, CenterAlt);
                FVector CornerPos = LlhToUnreal(Grid.MinLon, Grid.MinLat, Grid.MinHeight);
                
                // Extent 是半长宽 (Half-Extent)
                FVector Extent = (CenterPos - CornerPos).GetAbs(); 
                
                // [技巧] 稍微缩小一点点 (0.95)，这样相邻网格之间会有缝隙，视觉上更清晰
                Extent *= 0.95f; 

                // 物理检测
                bool bHit = CheckGridCollision(CenterPos, Extent);

                // =========================================================
                // [修改重点] 计算单个 Instance 的 Transform
                // =========================================================
                // 我们不再画12根线，而是画 1 个盒子。
                // Unreal 的标准 Cube 大小是 100x100x100。
                // 我们的 Extent 是半径。直径 = Extent * 2。
                // 所以 Scale = (Extent * 2) / 100 = Extent / 50。
                
                FVector Scale = Extent / 50.0f;
                FTransform GridTransform(FRotator::ZeroRotator, CenterPos, Scale);

                if (bHit)
                {
                    BatchOccupiedTransforms.Add(GridTransform);
                }
                else
                {
                    // 记录起始索引 (现在是 1对1 关系了，非常简单)
                    int32 StartIdx = NewPackage.FreeISM->GetInstanceCount() + BatchFreeTransforms.Num();
                    
                    BatchFreeTransforms.Add(GridTransform);

                    // 加入待重检列表
                    FSimpleGridInfo Info;
                    Info.Center = CenterPos;
                    Info.Extent = Extent;
                    Info.InstanceStartIndex = StartIdx; // 现在这里对应的就是一个 Instance
                    NewPackage.SuspectedFreeGrids.Add(Info);
                }
            }

            // 4. 提交 ISM 实例
            if (BatchFreeTransforms.Num() > 0) NewPackage.FreeISM->AddInstances(BatchFreeTransforms, false);
            if (BatchOccupiedTransforms.Num() > 0) NewPackage.OccupiedISM->AddInstances(BatchOccupiedTransforms, false);

            ChunkMap.Add(ChunkKey, NewPackage);
        });
    });
}

// =================================================================
// 核心逻辑：实时检测与状态修正 (升级版)
// =================================================================
void AGeoSOTISMManager::ProcessRealTimeDetection(const FVector& PlayerLocation, bool bCheckFarRegions)
{
    if (ChunkMap.Num() == 0) return;

    // 预计算距离平方，避免在循环中开方 (性能优化)
    double RadiusSq = HighPriorityRadius * HighPriorityRadius;

    // 遍历所有 Chunk
    for (auto& Pair : ChunkMap)
    {
        FChunkPackage& Package = Pair.Value;
        
        // 如果这个 Chunk 里没有"疑似空闲"的网格，直接跳过
        if (Package.SuspectedFreeGrids.Num() == 0) continue;

        TArray<int32> IndicesToRemove;

        // 倒序遍历 (为了方便后续移除)
        for (int32 i = Package.SuspectedFreeGrids.Num() - 1; i >= 0; --i)
        {
            const FSimpleGridInfo& Info = Package.SuspectedFreeGrids[i];

            // --- 步骤 1: 距离判断 ---
            double DistSq = FVector::DistSquared(Info.Center, PlayerLocation);
            bool bIsNear = DistSq < RadiusSq;

            // --- 步骤 2: 决策 - 这一帧该不该检测这个网格？---
            bool bShouldCheck = false;

            if (bIsNear)
            {
                // 情况 A: 网格在近处 (500m内)
                // 因为这个函数只在 NearTimer 触发时调用，所以近处网格总是会被检测
                bShouldCheck = true; 
            }
            else
            {
                // 情况 B: 网格在远处 (500m外)
                // 只有当 FarTimer 也触发了 (bCheckFarRegions == true) 才检测
                if (bCheckFarRegions)
                {
                    bShouldCheck = true;
                }
            }

            // 如果判定不需要检测，直接跳过，节省性能
            if (!bShouldCheck) continue;

            // --- 步骤 3: 物理检测 ---
            if (CheckGridCollision(Info.Center, Info.Extent))
            {
                // === 发现障碍：绿 -> 红 ===

                // A. 隐藏绿色实例 (将 Scale 设为 0)
                FTransform ZeroTransform;
                ZeroTransform.SetScale3D(FVector::ZeroVector);
                
                // 注意：这里我们假设一个网格对应一个 Instance (使用 Cube Mesh + Wireframe Material)
                Package.FreeISM->UpdateInstanceTransform(Info.InstanceStartIndex, ZeroTransform, true, false);

                // B. 在红色组件中新增一个实例
                // 重新计算 Scale: Extent 是半长宽，Cube 是 100，所以 Scale = Extent / 50
                FVector Scale = Info.Extent / 50.0f; 
                FTransform NewRedTransform(FRotator::ZeroRotator, Info.Center, Scale);
                
                Package.OccupiedISM->AddInstance(NewRedTransform, false);

                // C. 记录需要从列表中移除的索引
                IndicesToRemove.Add(i);
            }
        }

        // --- 步骤 4: 清理数据 ---
        if (IndicesToRemove.Num() > 0)
        {
            // 批量移除已变红的数据，以后不再检测它们
            for (int32 Index : IndicesToRemove)
            {
                Package.SuspectedFreeGrids.RemoveAtSwap(Index);
            }
            
            // 标记渲染状态更新
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
        FCollisionShape::MakeBox(Extent*0.95f),
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

