#include "ACesiumVoxelGridGenerator.h"
#include "DrawDebugHelpers.h" 
#include "Kismet/GameplayStatics.h" // 用于自动查找 Georeference
#include "Math/UnrealMathUtility.h"
#include "CollisionQueryParams.h" 
#include "Engine/World.h"

ACesiumVoxelGridGenerator::ACesiumVoxelGridGenerator()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ACesiumVoxelGridGenerator::BeginPlay()
{
    Super::BeginPlay();

    // 自动查找场景中的 CesiumGeoreference
    // 这样您就不必每次都手动拖拽赋值了
    if (!CesiumGeoreference)
    {
        AActor* FoundActor = UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass());
        CesiumGeoreference = Cast<ACesiumGeoreference>(FoundActor);

        if (CesiumGeoreference)
        {
            UE_LOG(LogTemp, Log, TEXT("CesiumGeoreference found automatically."));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("CesiumGeoreference NOT found! Please add one to the scene."));
        }
    }
}

void ACesiumVoxelGridGenerator::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    DrawVoxelGrid();
}

// ---------------------------------------------------------
// 参考 PlaneTrack.cpp 的转换逻辑
// ---------------------------------------------------------
FVector ACesiumVoxelGridGenerator::GetUnrealLocation(double Longitude, double Latitude, double Height) const
{
    // 安全检查：如果没有 Georeference，无法转换
    if (!CesiumGeoreference)
    {
        return FVector::ZeroVector;
    }

    // ---【完全参考 PlaneTrack 的写法】---
    // 使用 CesiumGeoreference 实例的方法进行转换
    // 输入：FVector(经度, 纬度, 高度)
    // 输出：UE 世界坐标
    return CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Longitude, Latitude, Height));
}

// ---------------------------------------------------------
// 网格绘制逻辑 (修正版 - 确保所有变量都已定义或使用)
// ---------------------------------------------------------
void ACesiumVoxelGridGenerator::DrawVoxelGrid()
{
    if (!CesiumGeoreference) return;

    UWorld* World = GetWorld();
    if (!World) return;

    // 三重循环：经度、纬度、高度
    for (double Lon = MinLongitude; Lon < MaxLongitude; Lon += StepLongitude)
    {
        for (double Lat = MinLatitude; Lat < MaxLatitude; Lat += StepLatitude)
        {
            for (double Alt = MinAltitude; Alt < MaxAltitude; Alt += StepAltitude)
            {
                // ---------------------------------------------------------
                // 1. 计算地理坐标 (P0_Geo 到 P7_Geo) - 假设这部分在省略号中
                // ---------------------------------------------------------
                double NextLon = Lon + StepLongitude;
                double NextLat = Lat + StepLatitude;
                double NextAlt = Alt + StepAltitude;

                // 转换为弧度
                double LonRad = FMath::DegreesToRadians(Lon);
                double LatRad = FMath::DegreesToRadians(Lat);
                double NextLonRad = FMath::DegreesToRadians(NextLon);
                double NextLatRad = FMath::DegreesToRadians(NextLat);

                // 创建8个角点的 Cartographic 对象
                // 底面 4 点
                CesiumGeospatial::Cartographic P0_Geo(LonRad, LatRad, Alt);
                CesiumGeospatial::Cartographic P1_Geo(NextLonRad, LatRad, Alt);
                CesiumGeospatial::Cartographic P2_Geo(NextLonRad, NextLatRad, Alt);
                CesiumGeospatial::Cartographic P3_Geo(LonRad, NextLatRad, Alt);

                // 顶面 4 点
                CesiumGeospatial::Cartographic P4_Geo(LonRad, LatRad, NextAlt);
                CesiumGeospatial::Cartographic P5_Geo(NextLonRad, LatRad, NextAlt);
                CesiumGeospatial::Cartographic P6_Geo(NextLonRad, NextLatRad, NextAlt);
                CesiumGeospatial::Cartographic P7_Geo(LonRad, NextLatRad, NextAlt);

                // ---------------------------------------------------------
                // 2. 转换为 Unreal 世界坐标
                // ---------------------------------------------------------
                FVector P0 = GetUnrealLocation(Lon, Lat, Alt);
                FVector P1 = GetUnrealLocation(NextLon, Lat, Alt);
                FVector P2 = GetUnrealLocation(NextLon, NextLat, Alt);
                FVector P3 = GetUnrealLocation(Lon, NextLat, Alt);
                FVector P4 = GetUnrealLocation(Lon, Lat, NextAlt);
                FVector P5 = GetUnrealLocation(NextLon, Lat, NextAlt);
                FVector P6 = GetUnrealLocation(NextLon, NextLat, NextAlt);
                FVector P7 = GetUnrealLocation(Lon, NextLat, NextAlt);


                // --- 【修正后】1. 计算体素中心点和半边长 ---

                // 计算体素的中心点 (Unreal 坐标)
                // 使用对角线中点 (P0 + P6) / 2.0f 计算中心，避免额外的 GetUnrealLocation 调用
                FVector CenterPoint = (P0 + P6) / 2.0f;

                // 计算体素的三个维度（X, Y, Z）的物理距离
                // Voxel宽度 (P0-P1 距离)
                float VoxelWidth = (P1 - P0).Size();
                // Voxel长度 (P0-P3 距离)
                float VoxelLength = (P3 - P0).Size();
                // Voxel高度 (P0-P4 距离)
                float VoxelHeight = (P4 - P0).Size();

                // 使用 Voxel 尺寸的一半作为盒体的半边长
                FVector HalfExtents(VoxelWidth / 2.0f, VoxelLength / 2.0f, VoxelHeight / 2.0f);

                // --- 2. 执行占据性检测 ---
                bool bOccupied = IsVoxelOccupied(CenterPoint, HalfExtents);

                // --- 3. 决定颜色 ---
                // Occupied (被占据) = 红色，Free (空闲) = 绿色
                FLinearColor CurrentColor = bOccupied ? FLinearColor::Red : FLinearColor::Green;
                FColor DrawColor = CurrentColor.ToFColor(true);

                // --- 4. 绘制线框（使用新颜色） ---

                // 绘制底面
                DrawDebugLine(World, P0, P1, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P1, P2, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P2, P3, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P3, P0, DrawColor, false, -1.0f, 0, LineThickness);

                // 绘制顶面
                DrawDebugLine(World, P4, P5, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P5, P6, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P6, P7, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P7, P4, DrawColor, false, -1.0f, 0, LineThickness);

                // 绘制立柱
                DrawDebugLine(World, P0, P4, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P1, P5, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P2, P6, DrawColor, false, -1.0f, 0, LineThickness);
                DrawDebugLine(World, P3, P7, DrawColor, false, -1.0f, 0, LineThickness);
            }
        }
    }
}

// ACesiumVoxelGridGenerator.cpp (在文件末尾添加函数实现)

bool ACesiumVoxelGridGenerator::IsVoxelOccupied(const FVector& Center, const FVector& HalfExtents) const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    // 用于存储碰撞结果的数组
    TArray<FOverlapResult> OverlapResults;

    // 忽略 Actor 本身 (防止 Actor 自己的碰撞体被检测到)
    FCollisionQueryParams Params;
    Params.AddIgnoredActor(this);

    // 设置碰撞通道：我们通常希望检测 WorldStatic (如 Cesium Tileset, 建筑)
    ECollisionChannel Channel = ECollisionChannel::ECC_WorldStatic;

    // 使用 OverlapMultiByChannel 进行多重盒体检测
    bool bHit = World->OverlapMultiByChannel(
        OverlapResults,
        Center,
        FQuat::Identity, // 盒体没有旋转
        Channel,
        FCollisionShape::MakeBox(HalfExtents), // 碰撞形状：一个盒子
        Params
    );

    // 如果 bHit 为真，且 OverlapResults 中有碰撞结果，则该体素被占据
    return bHit;
}