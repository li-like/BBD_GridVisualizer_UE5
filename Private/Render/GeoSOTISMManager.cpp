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
    NearTimer = 0.0f;
    FarTimer = 0.0f;
    LastUpdateLocation = FVector(0, 0, -99999999);

    // 默认加载 Cube
    static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeAsset(TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (CubeAsset.Succeeded())
    {
        BaseMesh = CubeAsset.Object;
    }
}

void AGeoSOTISMManager::BeginPlay()
{
    Super::BeginPlay();

    // 1. 获取 Georeference
    if (!Georeference)
    {
        Georeference = Cast<ACesiumGeoreference>(
            UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass()));
    }

    // 2. 获取 Cesium3DTileset
    if (!TargetTileset)
    {
        TargetTileset = Cast<ACesium3DTileset>(
            UGameplayStatics::GetActorOfClass(GetWorld(), ACesium3DTileset::StaticClass()));
    }

    // 3. 初始加载
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

    // --- 任务 A: 空间移动更新 (加载/卸载 Chunk) ---
    if (FVector::Dist(CurrentLoc, LastUpdateLocation) > 10000.0f) 
    {
        UpdateChunks(CurrentLoc);
        LastUpdateLocation = CurrentLoc;
    }

    // --- 任务 B: 实时重检 (近快远慢) ---
    NearTimer += DeltaTime;
    FarTimer += DeltaTime;

    if (NearTimer >= NearCheckInterval)
    {
        NearTimer = 0.0f;
        
        // 判断是否需要检查远处
        bool bCheckFar = false;
        if (FarTimer >= FarCheckInterval)
        {
            FarTimer = 0.0f;
            bCheckFar = true;
        }

        ProcessRealTimeDetection(CurrentLoc, bCheckFar);
    }
}

// =================================================================
// 空间管理
// =================================================================

void AGeoSOTISMManager::UpdateChunks(const FVector& PlayerLocation)
{
    if (bIsUpdatingChunks || !Georeference) return;
    bIsUpdatingChunks = true;

    FVector PlayerLLH = Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(PlayerLocation);
    double DegRadius = ViewRadius / UGeoSOTUtils::METERS_PER_DEGREE;
    
    FVector2D StartLL(PlayerLLH.X - DegRadius, PlayerLLH.Y - DegRadius);
    FVector2D EndLL(PlayerLLH.X + DegRadius, PlayerLLH.Y + DegRadius);

    // 获取视野内的所有大 Chunk
    TArray<FGeoSOTCell> NeededChunks = UGeoSOTUtils::GenerateGridsByRange3D(
        StartLL, EndLL, 0, 5000, ChunkLevel);

    TSet<int64> NewKeys;
    for (const FGeoSOTCell& Cell : NeededChunks)
    {
        int64 Key = ((int64)Cell.Code.Row << 32) | (int64)Cell.Code.Col;
        NewKeys.Add(Key);

        if (!ChunkMap.Contains(Key))
        {
            CreateChunk(Key, Cell);
        }
    }

    // 清理废弃 Chunk
    TArray<int64> KeysToRemove;
    for (auto& Elem : ChunkMap)
    {
        if (!NewKeys.Contains(Elem.Key))
        {
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
// 生成与渲染 (核心修复：旋转对齐 + 精确尺寸)
// =================================================================

void AGeoSOTISMManager::CreateChunk(int64 ChunkKey, const FGeoSOTCell& ChunkCell)
{
    if (!BaseMesh || !SafeWireMaterial || !ObstacleWireMaterial) return;

    double MinH = MinAltitude;
    double MaxH = LowAltitudeCeiling;
    int32 TargetLevel = RenderLevel;

    // Phase 1: 数学计算 (后台线程)
    AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, ChunkKey, ChunkCell, MinH, MaxH, TargetLevel]()
    {
        TArray<FGeoSOTCell> SubGrids = UGeoSOTUtils::GenerateGridsByRange3D(
            FVector2D(ChunkCell.MinLon, ChunkCell.MinLat),
            FVector2D(ChunkCell.MaxLon, ChunkCell.MaxLat),
            MinH, MaxH, TargetLevel);

        // Phase 2: 渲染与检测 (主线程)
        AsyncTask(ENamedThreads::GameThread, [this, ChunkKey, SubGrids]()
        {
            if (!IsValid(this)) return;

            // --- [关键 Step 1] 获取 Actor 当前的世界变换 ---
            // 我们需要用这个把计算出的世界坐标转回本地坐标，传给 ISM
            FTransform ActorTransform = GetActorTransform();
            
            // [调试信息] 打印 Actor 位置，观察是否被 Cesium 接管 (非0即被接管)
            UE_LOG(LogTemp, Warning, TEXT("[GeoSOT Debug] Chunk %lld Generating. Actor Loc: %s"), 
                ChunkKey, *ActorTransform.GetLocation().ToString());

            // 初始化组件包
            FChunkPackage NewPackage;
            
            // Helper Lambda: 创建 ISM
            auto CreateISM = [&](UMaterialInterface* Mat, FName Name) -> UInstancedStaticMeshComponent*
            {
                UInstancedStaticMeshComponent* Comp = NewObject<UInstancedStaticMeshComponent>(this, Name);
                Comp->RegisterComponent();
                Comp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
                Comp->SetStaticMesh(BaseMesh);
                Comp->SetMaterial(0, Mat);
                Comp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                Comp->SetCastShadow(false);
                return Comp;
            };

            NewPackage.FreeISM = CreateISM(SafeWireMaterial, FName(*FString::Printf(TEXT("Free_%lld"), ChunkKey)));
            NewPackage.OccupiedISM = CreateISM(ObstacleWireMaterial, FName(*FString::Printf(TEXT("Occ_%lld"), ChunkKey)));

            TArray<FTransform> BatchFreeTransforms;
            TArray<FTransform> BatchOccupiedTransforms;
            
            BatchFreeTransforms.Reserve(SubGrids.Num()); 
            NewPackage.SuspectedFreeGrids.Reserve(SubGrids.Num());

            // 用于控制日志频率 (只打第一个网格)
            bool bLoggedDebug = false;

            for (const FGeoSOTCell& Grid : SubGrids)
            {
                // === [核心解耦] 调用 Utils 获取世界变换 ===
                FVector CenterPos_World;
                FQuat GridQuat_World;
                FVector NewScale;

                // 这一步封装了所有复杂的几何计算
                if (!UGeoSOTUtils::CalculateGridWorldTransform(Georeference, Grid, CenterPos_World, GridQuat_World, NewScale))
                {
                    continue;
                }

                // === [新增] 获取唯一 ID ===
                int64 GridCode = UGeoSOTUtils::GetGridCodeID(Grid);

                // --- B. 转换为【本地空间】参数 (用于 ISM 渲染) ---
                // [核心修复] 使用 InverseTransformPosition
                FVector CenterPos_Local = ActorTransform.InverseTransformPosition(CenterPos_World);
                FQuat GridQuat_Local = ActorTransform.InverseTransformRotation(GridQuat_World);

                // 构建渲染用的 Transform (Local)
                FTransform RenderTransform(GridQuat_Local, CenterPos_Local, NewScale);

                // [调试信息] 输出第一条数据的坐标对比
                if (!bLoggedDebug)
                {
                    float OffsetDist = FVector::Dist(CenterPos_World, CenterPos_Local);
                    UE_LOG(LogTemp, Log, TEXT("   [Grid Debug] World: %s | Local: %s | Offset: %f"), 
                        *CenterPos_World.ToString(), *CenterPos_Local.ToString(), OffsetDist);
                    bLoggedDebug = true;
                }

                // --- C. 物理检测 (必须用世界坐标) ---
                FVector BoxExtent = NewScale * 50.0f; 
                bool bHit = CheckGridCollision(CenterPos_World, BoxExtent * 0.95f, GridQuat_World);

                if (bHit)
                {
                    BatchOccupiedTransforms.Add(RenderTransform);
                }
                else
                {
                    int32 StartIdx = NewPackage.FreeISM->GetInstanceCount() + BatchFreeTransforms.Num();
                    BatchFreeTransforms.Add(RenderTransform);

                    FSimpleGridInfo Info;
                    Info.GridCode = GridCode; // [新增] 存下 ID
                    Info.GridTransform = RenderTransform; // 存的是本地坐标！
                    Info.InstanceStartIndex = StartIdx;
                    NewPackage.SuspectedFreeGrids.Add(Info);
                }
            }

            if (BatchFreeTransforms.Num() > 0) NewPackage.FreeISM->AddInstances(BatchFreeTransforms, false);
            if (BatchOccupiedTransforms.Num() > 0) NewPackage.OccupiedISM->AddInstances(BatchOccupiedTransforms, false);

            ChunkMap.Add(ChunkKey, NewPackage);
        });
    });
}

// =================================================================
// 实时检测与状态修正
// =================================================================

void AGeoSOTISMManager::ProcessRealTimeDetection(const FVector& PlayerLocation, bool bCheckFarRegions)
{
    if (ChunkMap.Num() == 0) return;

    // 获取当前 Actor 的世界变换，用于将 Local 转回 World 进行检测
    FTransform ActorTransform = GetActorTransform();
    double RadiusSq = HighPriorityRadius * HighPriorityRadius;

    for (auto& Pair : ChunkMap)
    {
        FChunkPackage& Package = Pair.Value;
        if (Package.SuspectedFreeGrids.Num() == 0) continue;

        TArray<int32> IndicesToRemove;

        for (int32 i = Package.SuspectedFreeGrids.Num() - 1; i >= 0; --i)
        {
            const FSimpleGridInfo& Info = Package.SuspectedFreeGrids[i];
            
            // Info.GridTransform 存的是 Local Space
            FTransform LocalTransform = Info.GridTransform;

            // --- 1. 还原到世界空间 (用于距离判断和物理检测) ---
            // World = Local * Actor
            FTransform WorldTransform = LocalTransform * ActorTransform;
            
            FVector CenterPos_World = WorldTransform.GetLocation();

            // --- 2. 距离分级判断 ---
            double DistSq = FVector::DistSquared(CenterPos_World, PlayerLocation);
            bool bIsNear = DistSq < RadiusSq;
            
            // 如果既不是近处，且本帧也不查远处，就跳过
            if (!bIsNear && !bCheckFarRegions) continue;

            // --- 3. 物理检测 (使用世界空间参数) ---
            FQuat GridQuat_World = WorldTransform.GetRotation();
            FVector BoxExtent = WorldTransform.GetScale3D() * 50.0f; // Scale 直接乘

            if (CheckGridCollision(CenterPos_World, BoxExtent * 0.95f, GridQuat_World))
            {
                // === 发现障碍 ===

                // A. 隐藏绿色 (操作 ISM 使用 Local Transform)
                FTransform ZeroTransform;
                ZeroTransform.SetScale3D(FVector::ZeroVector);
                Package.FreeISM->UpdateInstanceTransform(Info.InstanceStartIndex, ZeroTransform, true, false);

                // B. 新增红色 (ISM AddInstance 默认就是 Local，所以直接传 Info.GridTransform)
                Package.OccupiedISM->AddInstance(LocalTransform, false);

                IndicesToRemove.Add(i);
            }
        }

        if (IndicesToRemove.Num() > 0)
        {
            for (int32 Index : IndicesToRemove)
            {
                Package.SuspectedFreeGrids.RemoveAtSwap(Index);
            }
            Package.FreeISM->MarkRenderStateDirty();
            Package.OccupiedISM->MarkRenderStateDirty();
        }
    }
}

// =================================================================
// 工具函数
// =================================================================

bool AGeoSOTISMManager::CheckGridCollision(const FVector& Center, const FVector& BoxExtent, const FQuat& Rotation) const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    FCollisionQueryParams Params;
    Params.bTraceComplex = true; // 必须开启
    Params.AddIgnoredActor(this);
    
    APawn* Player = UGameplayStatics::GetPlayerPawn(World, 0);
    if (Player) Params.AddIgnoredActor(Player);

    // 【核心修复】传入 Rotation 进行 OBB 检测
    return World->OverlapBlockingTestByChannel(
        Center,
        Rotation, 
        ECC_WorldStatic, 
        FCollisionShape::MakeBox(BoxExtent), 
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