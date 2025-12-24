// Copyright 2020-2021 CesiumGS, Inc. and Contributors


#include "ABDBasedMultiResGridVisualizer.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h" 
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"

// Sets default values
AABDBasedMultiResGridVisualizer::AABDBasedMultiResGridVisualizer()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

}

// Called when the game starts or when spawned
void AABDBasedMultiResGridVisualizer::BeginPlay()
{
	Super::BeginPlay();

    // 自动寻找 CesiumGeoreference
    if (!CesiumGeoreference)
    {
        AActor* FoundActor = UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass());
        CesiumGeoreference = Cast<ACesiumGeoreference>(FoundActor);
    }

    // 初始化渲染组件
    SetupISMComponents();
	
}


/// 初始化渲染组件
void AABDBasedMultiResGridVisualizer::SetupISMComponents()
{
    if (LevelISMs.Num() > 0) return;

    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!CubeMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("Failed to load Cube Mesh!"));
        return;
    }

    for (int32 i = 0; i < 10; ++i)
    {
        FString CompName = FString::Printf(TEXT("ISM_Level_%d"), i + 1);
        UInstancedStaticMeshComponent* ISM = NewObject<UInstancedStaticMeshComponent>(this, FName(*CompName));

        if (ISM)
        {
            ISM->RegisterComponent();
            ISM->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
            ISM->SetStaticMesh(CubeMesh);
            ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            ISM->SetCastShadow(false);

            // 材质分配 (请确保在编辑器中赋值了 MI_Clean_Low 等材质)
            int32 Level = i + 1;
            if (Level <= 5 && MaterialLow) ISM->SetMaterial(0, MaterialLow);
            else if (Level == 6 && MaterialMid) ISM->SetMaterial(0, MaterialMid);
            else if (Level >= 7 && MaterialHigh) ISM->SetMaterial(0, MaterialHigh);

            LevelISMs.Add(ISM);
        }
    }
    // --- L7 Free (绿色) ---
    ISM_L7_Free = NewObject<UInstancedStaticMeshComponent>(this, TEXT("ISM_L7_Free"));
    if (ISM_L7_Free)
    {
        ISM_L7_Free->RegisterComponent();
        ISM_L7_Free->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
        ISM_L7_Free->SetStaticMesh(CubeMesh);
        ISM_L7_Free->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ISM_L7_Free->SetCastShadow(false);
        // 如果用户在编辑器设置了 MaterialFree，就用它；否则用默认绿
        if (MaterialFree) ISM_L7_Free->SetMaterial(0, MaterialFree);
    }

    // --- L7 Occupied (红色) ---
    ISM_L7_Occupied = NewObject<UInstancedStaticMeshComponent>(this, TEXT("ISM_L7_Occupied"));
    if (ISM_L7_Occupied)
    {
        ISM_L7_Occupied->RegisterComponent();
        ISM_L7_Occupied->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
        ISM_L7_Occupied->SetStaticMesh(CubeMesh);
        ISM_L7_Occupied->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ISM_L7_Occupied->SetCastShadow(false);
        if (MaterialOccupied) ISM_L7_Occupied->SetMaterial(0, MaterialOccupied);
    }
}

// Called every frame
void AABDBasedMultiResGridVisualizer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

    // 获取相机位置
    FVector CameraLoc = UGameplayStatics::GetPlayerPawn(this, 0)->GetActorLocation();

    // 如果是第一次运行，或者 移动距离超过了阈值，才重新计算
    if (!bInitialized || FVector::Dist(CameraLoc, LastUpdateLocation) > UpdateDistanceThreshold)
    {
        // 更新记录点
        LastUpdateLocation = CameraLoc;
        bInitialized = true;

        // 执行繁重的计算任务
        DrawMultiResGrid();

        UE_LOG(LogTemp, Warning, TEXT("Grid Updated at: %s"), *LastUpdateLocation.ToString());
    }

}

// 辅助：获取 Unreal 坐标
FVector AABDBasedMultiResGridVisualizer::GetUnrealLocation(double Longitude, double Latitude, double Height) const
{
    if (CesiumGeoreference)
    {
        return CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Longitude, Latitude, Height));
    }
    return FVector::ZeroVector;
}

// 北斗拆分比例表
// 依据之前的逻辑：L5->L6 (2x2), L6->L7 (8x8)
bool AABDBasedMultiResGridVisualizer::GetBeidouSplitRatio(int32 CurrentLevel, int32& OutLonDiv, int32& OutLatDiv, int32& OutAltDiv) const
{
    switch (CurrentLevel)
    {
    case 5: OutLonDiv = 2;  OutLatDiv = 2;  OutAltDiv = 2;  return true; // L5(4") -> L6(2")
    case 6: OutLonDiv = 8;  OutLatDiv = 8;  OutAltDiv = 8;  return true; // L6(2") -> L7(0.25"/UserDefined)
        // 其他层级虽然用不到，但为了完整性保留
    default: OutLonDiv = 2; OutLatDiv = 2; OutAltDiv = 2; return true;
    }
}



// 密度计算 (蒙特卡洛采样)
float AABDBasedMultiResGridVisualizer::CalculateRegionDensity(const FVector& Center, const FVector& Extent, int32 SampleCount) const
{
    UWorld* World = GetWorld();
    if (!World) return 0.0f;

    FCollisionQueryParams Params;
    Params.AddIgnoredActor(this);

    // 1. 快速初筛
    bool bRoughHit = World->OverlapBlockingTestByChannel(
        Center,
        FQuat::Identity,
        ECollisionChannel::ECC_WorldStatic,
        FCollisionShape::MakeBox(Extent),
        Params
    );

    if (!bRoughHit) return 0.0f;

    // 2. 精细采样
    int32 HitCount = 0;
    FRandomStream Stream(GetTypeHash(Center));

    for (int32 i = 0; i < SampleCount; ++i)
    {
        float RandX = Stream.FRandRange(-1.0f, 1.0f);
        float RandY = Stream.FRandRange(-1.0f, 1.0f);
        float RandZ = Stream.FRandRange(-1.0f, 1.0f);
        FVector RandomPoint = Center + FVector(RandX, RandY, RandZ) * Extent;

        bool bPointHit = World->OverlapBlockingTestByChannel(
            RandomPoint,
            FQuat::Identity,
            ECollisionChannel::ECC_WorldStatic,
            FCollisionShape::MakeSphere(2.0f),
            Params
        );

        if (bPointHit) HitCount++;
    }

    return (float)HitCount / (float)SampleCount;
}







// [环境感知] 物理检测
bool AABDBasedMultiResGridVisualizer::IsRegionDense(const FVector& Center, const FVector& Extent) const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    FCollisionQueryParams Params;
    Params.AddIgnoredActor(this); // 忽略自己
    Params.bTraceComplex = true;  // 【关键】开启复杂检测，针对 3D Tiles 的三角面片必须开这个

    // =========================================================
    // 【修复 Bug】 忽略玩家角色
    // =========================================================
    // 获取当前玩家控制的角色 (Pawn)
    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0);
    if (PlayerPawn)
    {
        // 将其加入忽略列表
        Params.AddIgnoredActor(PlayerPawn);
    }

    FHitResult HitResult;

    // 使用 Sweep (扫描) 代替 Overlap
    // 原理：在原地微小移动盒子，检测是否碰到任何东西
    bool bHit = World->SweepSingleByChannel(
        HitResult,
        Center,
        Center + FVector(0, 0, 1.0f), // 稍微移动 1cm
        FQuat::Identity,
        ECollisionChannel::ECC_WorldStatic, // 确保 Cesium 的 Collision Presets 包含这个
        FCollisionShape::MakeBox(Extent),
        Params
    );

    // --- 调试可视化 (调试完后请注释掉) ---
    // 如果碰到了(红色) = 应该消失
    // 如果没碰到(绿色) = 应该显示
    //DrawDebugBox(World, Center, Extent, bHit ? FColor::Red : FColor::Green, false, 0.1f, 0, 2.0f);
    // ----------------------------------

    return bHit;
}

void AABDBasedMultiResGridVisualizer::ProcessGridRecursive(
    const FVector2D& LonRange,
    const FVector2D& LatRange,
    const FVector2D& AltRange,
    int32 CurrentLevel,
    int32 FinalTargetLevel,
    const FVector& ViewerPos)
{
    // 1. 计算当前网格几何信息
    double CenterLon = (LonRange.X + LonRange.Y) / 2.0;
    double CenterLat = (LatRange.X + LatRange.Y) / 2.0;
    double CenterAlt = (AltRange.X + AltRange.Y) / 2.0;
    FVector CenterPos = GetUnrealLocation(CenterLon, CenterLat, CenterAlt);
    FVector CornerPos = GetUnrealLocation(LonRange.X, LatRange.X, AltRange.X);
    FVector Extent = (CenterPos - CornerPos).GetAbs();


    // =========================================================
    // 【新增】距离剔除逻辑
    // =========================================================
    float DistToViewer = FVector::Dist(CenterPos, ViewerPos);

    // 定义视距标准 (单位: cm)
    const float L7_Visible_Radius = 20000.0f;  // 800米以内才允许分到 L7
    const float L6_Visible_Radius = 300000.0f; // 3000米以内允许分到 L6

    // 动态调整目标层级 (Level of Detail)
    int32 DynamicTargetLevel = FinalTargetLevel;

    // 如果太远了，强制降低目标层级
    if (DistToViewer > L6_Visible_Radius)
    {
        // 极远距离：只看 L5，不要细分了
        if (CurrentLevel >= 5) return; // 甚至可以直接 return，只画 L5
    }
    else if (DistToViewer > L7_Visible_Radius)
    {
        // 中远距离：最多分到 L6，禁止分到 L7
        if (DynamicTargetLevel > 6) DynamicTargetLevel = 6;
    }

    // =========================================================
    // 阶段 A: 宏观决策 (仅在 L5 根节点执行)
    // =========================================================
    if (CurrentLevel == 5)
    {
        // 快速判断该区域是否有障碍物
        bool bHasBuilding = IsRegionDense(CenterPos, Extent);

        int32 DecidedTarget = 6; // 默认 L6

        if (bHasBuilding)
        {
            // 有建筑 -> 下放 L7 (为了去剔除它)
            DecidedTarget = 7;
        }
        else
        {
            // 空旷区域
            // 如果高度 < 60m -> 强制下放 L7 (显示低空空域)
            if (CenterAlt < 60.0)
            {
                DecidedTarget = 7;
            }
            else
            {
                // 高空空旷 -> 保留 L6 或 L5
                if (CenterAlt > 200.0) DecidedTarget = 5;
                else DecidedTarget = 6;
            }
        }

        // 【关键】再次应用距离限制
        // 即使决策说要分到 L7，但如果距离太远，我们驳回，强制改为 6
        if (DistToViewer > L7_Visible_Radius && DecidedTarget > 6)
        {
            DecidedTarget = 6;
        }

        // 如果决定保留 L5，就不切分了
        if (DecidedTarget == 5)
        {
            // 直接绘制 L5
        }
        else
        {
            // 执行切分
            int32 LonDivs, LatDivs, AltDivs;
            if (GetBeidouSplitRatio(CurrentLevel, LonDivs, LatDivs, AltDivs))
            {
                double LonStep = (LonRange.Y - LonRange.X) / LonDivs;
                double LatStep = (LatRange.Y - LatRange.X) / LatDivs;
                double AltStep = (AltRange.Y - AltRange.X) / AltDivs;

                for (int i = 0; i < LonDivs; ++i)
                    for (int j = 0; j < LatDivs; ++j)
                        for (int k = 0; k < AltDivs; ++k)
                        {
                            ProcessGridRecursive(
                                FVector2D(LonRange.X + i * LonStep, LonRange.X + (i + 1) * LonStep),
                                FVector2D(LatRange.X + j * LatStep, LatRange.X + (j + 1) * LatStep),
                                FVector2D(AltRange.X + k * AltStep, AltRange.X + (k + 1) * AltStep),
                                CurrentLevel + 1,
                                DecidedTarget,
                                ViewerPos
                            );
                        }
            }
            return; // 任务已下发
        }
    }

    // =========================================================
    // 阶段 B: 中间层级递归
    // =========================================================
    if (CurrentLevel < FinalTargetLevel)
    {
        int32 LonDivs, LatDivs, AltDivs;
        if (GetBeidouSplitRatio(CurrentLevel, LonDivs, LatDivs, AltDivs))
        {
            double LonStep = (LonRange.Y - LonRange.X) / LonDivs;
            double LatStep = (LatRange.Y - LatRange.X) / LatDivs;
            double AltStep = (AltRange.Y - AltRange.X) / AltDivs;

            for (int i = 0; i < LonDivs; ++i)
                for (int j = 0; j < LatDivs; ++j)
                    for (int k = 0; k < AltDivs; ++k)
                    {
                        ProcessGridRecursive(
                            FVector2D(LonRange.X + i * LonStep, LonRange.X + (i + 1) * LonStep),
                            FVector2D(LatRange.X + j * LatStep, LatRange.X + (j + 1) * LatStep),
                            FVector2D(AltRange.X + k * AltStep, AltRange.X + (k + 1) * AltStep),
                            CurrentLevel + 1,
                            FinalTargetLevel,
                            ViewerPos
                        );
                    }
        }
        return;
    }

    // =========================================================
    // 阶段 C: 渲染 (与剔除逻辑)
    // =========================================================

    FVector Dimensions = Extent * 2.0f;
    FVector Scale = Dimensions / 100.0f;
    FTransform InstanceTransform(FQuat::Identity, CenterPos, Scale);

    // 【L7 特殊处理】：分流到 Free(绿) 和 Occupied(红)
    if (CurrentLevel == 7)
    {
        // 核心检测：这里是障碍吗？
        bool bIsOccupied = IsRegionDense(CenterPos, Extent*1.5f);

        if (bIsOccupied)
        {
            // 如果碰到了 -> 画红色 (障碍)
            // 如果你想"剔除"它，直接注释掉下面这行即可。
            // 但为了调试，强烈建议先画出来，看看红框是不是真的对准了建筑物。
            if (ISM_L7_Occupied) ISM_L7_Occupied->AddInstance(InstanceTransform, true);
        }
        else
        {
            // 没碰到 -> 画绿色 (Free)
            if (ISM_L7_Free) ISM_L7_Free->AddInstance(InstanceTransform, true);
        }
    }
    // L5, L6 常规渲染
    else
    {
        if (LevelISMs.IsValidIndex(CurrentLevel - 1))
        {
            LevelISMs[CurrentLevel - 1]->AddInstance(InstanceTransform, true);
        }
    }
}


// 绘制入口：修改为 L5 遍历
void AABDBasedMultiResGridVisualizer::DrawMultiResGrid()
{
    // 1. 清空旧数据
    for (auto* ISM : LevelISMs) { if (ISM) ISM->ClearInstances(); }
    if (ISM_L7_Free) ISM_L7_Free->ClearInstances();
    if (ISM_L7_Occupied) ISM_L7_Occupied->ClearInstances();

    // 2. 获取相机位置并转换为经纬度
    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!PlayerPawn || !CesiumGeoreference) return;

    FVector CameraLoc = PlayerPawn->GetActorLocation();

    // 使用 Cesium API 将 Unreal 坐标转回 经度/纬度/高度
    // 注意：返回值通常是 X=Longitude, Y=Latitude, Z=Height
    FVector LongLatHeight = CesiumGeoreference->TransformUnrealPositionToLongitudeLatitudeHeight(CameraLoc);
    double CamLon = LongLatHeight.X;
    double CamLat = LongLatHeight.Y;

    // 3. 定义扫描窗口 (半径)
    // 经度 1度 ≈ 111km (赤道)。
    // 0.01度 ≈ 1.1km。
    // 设置半径为 0.01度，意味着生成一个 2.2km x 2.2km 的方块区域。
    const double ScanRadius = 0.01;

    // 计算原始的窗口边界 (并限制在全局最大范围内)
    double RawStartLon = FMath::Max(MinLongitude, CamLon - ScanRadius);
    double RawEndLon = FMath::Min(MaxLongitude, CamLon + ScanRadius);
    double RawStartLat = FMath::Max(MinLatitude, CamLat - ScanRadius);
    double RawEndLat = FMath::Min(MaxLatitude, CamLat + ScanRadius);

    // 4. 网格对齐 (Snap to Grid) - 【防抖动关键】
    // 我们必须确保循环的起点永远落在 L5 的整数倍网格线上。
    // 如果直接用 CamLon - Radius 作为起点，网格会随着相机微动而产生浮点数误差抖动。
    const int32 RootLevel = 5;
    const double StepL5 = 4.0 / 3600.0; // L5 步长 (4秒)

    double StartLon = FMath::FloorToDouble(RawStartLon / StepL5) * StepL5;
    double StartLat = FMath::FloorToDouble(RawStartLat / StepL5) * StepL5;

    // 5. 开始循环 (只循环窗口内的范围)
    // 注意循环条件是用 RawEndLon
    for (double Lon = StartLon; Lon < RawEndLon; Lon += StepL5)
    {
        for (double Lat = StartLat; Lat < RawEndLat; Lat += StepL5)
        {
            // 扫描高度 0 - 360m (3层 L5)
            // 如果您只需要看低空，甚至可以把高度范围也缩小
            for (int h = 0; h < 3; ++h)
            {
                double BaseAlt = h * 120.0;

                // 传入 CameraLoc 给递归函数做距离 LOD 判断 (上一轮我们加的参数)
                ProcessGridRecursive(
                    FVector2D(Lon, Lon + StepL5),
                    FVector2D(Lat, Lat + StepL5),
                    FVector2D(BaseAlt, BaseAlt + 120.0),
                    RootLevel,
                    -1,
                    CameraLoc // 传入相机位置
                );
            }
        }
    }
}

