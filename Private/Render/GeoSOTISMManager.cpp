#include "Render/GeoSOTISMManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Core/MyGeoSOTDatabase.h"
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
    
    // [新增] 初始化路径 ISM
    PathISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("PathISM"));
    PathISM->SetupAttachment(RootComponent);
    PathISM->SetCollisionEnabled(ECollisionEnabled::NoCollision); // 路径只是看的，不需要碰撞
    PathISM->SetCastShadow(false); // 路径通常发光，不需要投影
}

// 2. 实现清除函数
void AGeoSOTISMManager::ClearPath()
{
    if (PathISM)
    {
        PathISM->ClearInstances();
    }
}

// 3. 实现核心渲染函数
void AGeoSOTISMManager::DrawPath(const TArray<int64>& PathCodes)
{
    if (!PathISM) return;
    
    // 先清空旧路径
    ClearPath();

    if (PathCodes.Num() == 0) return;

    // 2. 计算缩放 (Scale)
    // 假设 L21 层级，动态计算尺寸
    // 注意：PathCodes[0] 可能是任意层级，最好取第一个点的层级来算
    int32 R, C, Level, Alt;
    UGeoSOTUtils::DecodeGeoSOTCode(PathCodes[0], R, C, Level, Alt);
    
    double GridSizeMeters = UGeoSOTUtils::GetHeightSpan(Level); 
    double GridSizeUE = GridSizeMeters * 100.0; // 厘米

    // 假设 BaseMesh 是 100x100x100 的立方体
    FVector InstanceScale(GridSizeUE / 100.0);
    
    UE_LOG(LogTemp, Warning, TEXT("========== ISM DRAW PATH DEBUG =========="));
    UE_LOG(LogTemp, Warning, TEXT("Level: %d | HSpan: %.2f meters"), Level, GridSizeMeters);

    for (int64 Code : PathCodes)
    {
        // 1. 利用我们上一轮写好的逆变换函数获取中心点
        FVector Location = GetWorldLocationFromGridCode(Code);

        // 2. 构造 Transform
        FTransform InstanceTransform;
        InstanceTransform.SetLocation(Location);
        InstanceTransform.SetRotation(FQuat::Identity);
        InstanceTransform.SetScale3D(InstanceScale);
        
        UE_LOG(LogTemp, Warning, TEXT("Code=%lld AltIndex=%d | RenderZ_UE=%.2f | RenderZ_Meters=%.2f"), 
             Code ,Alt, Location.Z, Location.Z / 100.0f);

        // 3. 添加实例
        PathISM->AddInstance(InstanceTransform, true);
    }
    UE_LOG(LogTemp, Warning, TEXT("========================================="));
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
            
            // === [新增 1] 获取数据库子系统 ===
            UMyGeoSOTDatabase* DB = nullptr;
            if (bIsBakingMode) // 只有开启烘焙模式才连接数据库
            {
                DB = GetGameInstance()->GetSubsystem<UMyGeoSOTDatabase>();
                if (DB) DB->BeginTransaction(); // [关键] 开启事务！否则写入几千条会卡顿数秒
            }

            // --- [关键 Step 1] 获取 Actor 当前的世界变换 ---
            // 我们需要用这个把计算出的世界坐标转回本地坐标，传给 ISM
            FTransform ActorTransform = GetActorTransform();
            
            // // [调试信息] 打印 Actor 位置，观察是否被 Cesium 接管 (非0即被接管)
            // UE_LOG(LogTemp, Warning, TEXT("[GeoSOT Debug] Chunk %lld Generating. Actor Loc: %s"), 
            //     ChunkKey, *ActorTransform.GetLocation().ToString());

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
            int32 DebugCount = 0;

            for (const FGeoSOTCell& Grid : SubGrids)
            {
                // === [核心解耦] 调用 Utils 获取世界变换 ===
                FVector CenterPos_World;
                FQuat GridQuat_World;
                FVector NewScale;
                double GridHeightSpan = Grid.MaxHeight - Grid.MinHeight;
                int32 AltLayerIndex = 0;
                if (GridHeightSpan > 0.1) // 防止除以0
                {
                    AltLayerIndex = FMath::RoundToInt((Grid.MinHeight - MinAltitude) / GridHeightSpan);
                }

                // 这一步封装了所有复杂的几何计算
                if (!UGeoSOTUtils::CalculateGridWorldTransform(Georeference, Grid, CenterPos_World, GridQuat_World, NewScale))
                {
                    continue;
                }

                // === [新增] 获取唯一 ID ===
                int64 GridCode = UGeoSOTUtils::GenerateGeoSOTCode(Grid.Code.Row, Grid.Code.Col, Grid.Code.Level,AltLayerIndex);
                // ================= [调试代码开始] =================
                // 每个 Chunk 只打印前 10 个网格的编码详情
                // if (DebugCount < 10)
                // {
                //     // 将 Code 转为二进制字符串，方便检查位交叉是否正确
                //     FString BinaryStr = "";
                //     for (int32 i = 63; i >= 0; --i)
                //     {
                //         BinaryStr += ((GridCode >> i) & 1) ? TEXT("1") : TEXT("0");
                //     }
                //
                //     UE_LOG(LogTemp, Warning, TEXT("[GeoSOT Check] Level: %d | Row: %d | Col: %d => Code: %lld"), 
                //         Grid.Code.Level, Grid.Code.Row, Grid.Code.Col, GridCode);
                //     
                //     // 如果你想看更详细的二进制结构，取消下面这行的注释
                //     UE_LOG(LogTemp, Log, TEXT("    Binary: %s"), *BinaryStr);
                //
                //     DebugCount++;
                // }
                // ================= [调试代码结束] =================

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
                    // UE_LOG(LogTemp, Log, TEXT("   [Grid Debug] World: %s | Local: %s | Offset: %f"), 
                    //     *CenterPos_World.ToString(), *CenterPos_Local.ToString(), OffsetDist);
                    bLoggedDebug = true;
                }

                // --- C. 物理检测 (必须用世界坐标) ---
                FVector BoxExtent = NewScale * 50.0f; 
                bool bHit = CheckGridCollision(CenterPos_World, BoxExtent * 1.2f, GridQuat_World);

                if (bHit)
                {
                    
                    // === [新增 3] 存入数据库 ===
                    if (bIsBakingMode && DB)
                    {
                        // 存入 Code, Level, AltLayer
                        DB->SaveObstacle(GridCode, Grid.Code.Level, AltLayerIndex);
                    }
                    // 红色
                    int32 NewIdx = NewPackage.OccupiedISM->GetInstanceCount() + BatchOccupiedTransforms.Num();
                    BatchOccupiedTransforms.Add(RenderTransform);
                    
                    // [新增] 记录红色映射
                    NewPackage.OccupiedInstanceToCodeMap.Add(NewIdx, GridCode);
                }
                else
                {
                    // 绿色
                    int32 NewIdx = NewPackage.FreeISM->GetInstanceCount() + BatchFreeTransforms.Num();
                    BatchFreeTransforms.Add(RenderTransform);

                    FSimpleGridInfo Info;
                    Info.GridCode = GridCode; // 记得存到 Info 里
                    Info.GridTransform = RenderTransform;
                    Info.InstanceStartIndex = NewIdx;
                    NewPackage.SuspectedFreeGrids.Add(Info);

                    // [新增] 记录绿色映射
                    NewPackage.FreeInstanceToCodeMap.Add(NewIdx, GridCode);
                }
            }
            
            // === [新增 4] 提交事务 ===
            if (bIsBakingMode && DB)
            {
                DB->CommitTransaction(); // 一次性把这几百条数据写入磁盘
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
                int32 NewOccupiedIndex = Package.OccupiedISM->GetInstanceCount();
                
                FTransform ZeroTransform;
                ZeroTransform.SetScale3D(FVector::ZeroVector);
                Package.FreeISM->UpdateInstanceTransform(Info.InstanceStartIndex, ZeroTransform, true, false);

                // B. 新增红色 (ISM AddInstance 默认就是 Local，所以直接传 Info.GridTransform)
                int32 ActualNewIndex = Package.OccupiedISM->AddInstance(LocalTransform, false);
                Package.OccupiedInstanceToCodeMap.Add(ActualNewIndex, Info.GridCode);

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

// 1. 世界坐标 -> GeoSOT 编码 (World -> Code)
int64 AGeoSOTISMManager::GetGridCodeFromWorldLocation(FVector WorldLoc)
{
    // 检查 Cesium 引用
    if (!Georeference) 
    {
        return -1;
    }

    // Step 1: UE 坐标 -> 经纬度高度 (Lon, Lat, Height)
    FVector3d LLH = Georeference->TransformUnrealPositionToLongitudeLatitudeHeight(FVector3d(WorldLoc));

    double Lon = LLH.X;
    double Lat = LLH.Y;
    double Height = LLH.Z;

    // Step 2: 经纬度 -> 2D 索引 (Row, Col)
    // 统一使用 L21 进行规划 (或者你可以将 TargetLevel 提升为参数)
    int32 TargetLevel = 21; 
    int32 Row, Col;
    UGeoSOTUtils::LatLonToGeoSOT(Lat, Lon, TargetLevel, Row, Col);

    // Step 3: 高度 -> 高度层索引 (AltLayer)
    // [关键修正] 调用 Utils 中已存在的标准函数获取层高
    double CurrentLayerSpan = UGeoSOTUtils::GetHeightSpan(TargetLevel);

    // 高度检查
    if (Height < MinAltitude) 
    {
        // 如果低于基准高度，返回无效
        return -1; 
    }

    // 计算层号
    int32 AltLayer = FMath::FloorToInt((Height - MinAltitude) / CurrentLayerSpan);

    // Step 4: 生成编码
    return UGeoSOTUtils::GenerateGeoSOTCode(Row, Col, TargetLevel, AltLayer);
}

// 2. GeoSOT 编码 -> 世界坐标 (Code -> World)
FVector AGeoSOTISMManager::GetWorldLocationFromGridCode(int64 GridCode)
{
    if (!Georeference) return FVector::ZeroVector;

    // Step 1: 解码
    int32 Row, Col, Level, AltLayer;
    UGeoSOTUtils::DecodeGeoSOTCode(GridCode, Row, Col, Level, AltLayer);

    // Step 2: 还原经纬度中心
    double CenterLat, CenterLon;
    UGeoSOTUtils::GetGeoSOTCenter(Row, Col, Level, CenterLat, CenterLon);

    // Step 3: 还原高度中心
    // [关键修正] 使用 Level 对应的标准高度跨度
    double CurrentLayerSpan = UGeoSOTUtils::GetHeightSpan(Level);

    // 高度 = 基准 + (层号 + 0.5) * 层高
    // +0.5 是为了取体素的几何中心
    double CenterHeight = MinAltitude + ((double)AltLayer + 0.5) * CurrentLayerSpan;

    // Step 4: 转回 UE 坐标
    // Cesium 输入顺序: Lon, Lat, Height
    FVector3d WorldPos = Georeference->TransformLongitudeLatitudeHeightPositionToUnreal(
        FVector3d(CenterLon, CenterLat, CenterHeight)
    );

    return (FVector)WorldPos;
}


