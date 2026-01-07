#include "Core/GeoSOTUtils.h"
#include <cmath>
#include "CesiumGeoreference.h"

double UGeoSOTUtils::GetDegSpan(int32 Level)
{
    // GeoSOT 公式：512 / 2^N，传入相对应的等级，获取到该等级所对应的度数
    return GEOSOT_MAX_RANGE / std::pow(2.0, Level);
}

double UGeoSOTUtils::GetHeightSpan(int32 Level)
{
    // 逻辑：高度维的"1度" = 赤道上的"1度物理距离"
    // Level 15: (512 / 2^15) * 111319 ≈ 0.015625 * 111319 ≈ 1739 米
    // 这与您的表格 (1.8km) 符合
    double GridSpanDeg = GetDegSpan(Level);
    return GridSpanDeg * METERS_PER_DEGREE;
}

int64 UGeoSOTUtils::CalculateLatLonIndex(double Degree, int32 Level)
{
    double Span = GetDegSpan(Level);
    // 偏移到 [0, 512] 空间
    double OffsetVal = Degree + GEOSOT_OFFSET;
    return static_cast<int64>(std::floor(OffsetVal / Span));
}

int64 UGeoSOTUtils::CalculateAltIndex(double HeightMeters, int32 Level)
{
    double HSpan = GetHeightSpan(Level);
    // 简单的高度索引：Height / Span
    // GeoSOT 标准可能有一个巨大的 Z 轴 Offset (例如地心为0)，
    // 但为了适配 Unreal/Cesium 的 WGS84 高度 (海平面=0)，我们直接基于 0 计算即可。
    // 支持负高度 (Index 会是负数)。
    return static_cast<int64>(std::floor(HeightMeters / HSpan));
}

TArray<FGeoSOTCell> UGeoSOTUtils::GenerateGridsByRange3D(
    FVector2D StartLonLat, 
    FVector2D EndLonLat, 
    double MinHeight, 
    double MaxHeight, 
    int32 Level)
{
    TArray<FGeoSOTCell> ResultGrids;
    if (Level < 0 || Level > 32) return ResultGrids;

    // 1. 规范化经纬度范围
    double LatMin = FMath::Min(StartLonLat.Y, EndLonLat.Y);
    double LatMax = FMath::Max(StartLonLat.Y, EndLonLat.Y);
    double LonMin = FMath::Min(StartLonLat.X, EndLonLat.X);
    double LonMax = FMath::Max(StartLonLat.X, EndLonLat.X);

    // 2. 规范化高度范围
    double HMin = FMath::Min(MinHeight, MaxHeight);
    double HMax = FMath::Max(MinHeight, MaxHeight);

    // 3. 计算索引范围
    int64 RowStart = CalculateLatLonIndex(LatMin, Level);
    int64 RowEnd   = CalculateLatLonIndex(LatMax, Level);
    int64 ColStart = CalculateLatLonIndex(LonMin, Level);
    int64 ColEnd   = CalculateLatLonIndex(LonMax, Level);
    int64 AltStart = CalculateAltIndex(HMin, Level);
    int64 AltEnd   = CalculateAltIndex(HMax, Level);

    // 4. 获取跨度
    double DegSpan = GetDegSpan(Level);
    double HSpan = GetHeightSpan(Level);

    // 5. 三层循环生成体素
    for (int64 r = RowStart; r <= RowEnd; r++)
    {
        for (int64 c = ColStart; c <= ColEnd; c++)
        {
            for (int64 a = AltStart; a <= AltEnd; a++)
            {
                FGeoSOTCell Cell;
                Cell.Code = FGeoSOTCode(Level, r, c, a);

                // 反算经纬度
                Cell.MinLon = (c * DegSpan) - GEOSOT_OFFSET;
                Cell.MaxLon = Cell.MinLon + DegSpan;
                Cell.MinLat = (r * DegSpan) - GEOSOT_OFFSET;
                Cell.MaxLat = Cell.MinLat + DegSpan;

                // 反算高度 (核心修正点)
                // 网格的高度不再是用户输入的 Min/Max，而是由索引决定的标准高度
                Cell.MinHeight = a * HSpan;
                Cell.MaxHeight = Cell.MinHeight + HSpan;

                ResultGrids.Add(Cell);
            }
        }
    }

    return ResultGrids;
}

TArray<FGeoSOTCell> UGeoSOTUtils::GetGridsInRadius3D(
    double CenterLon, double CenterLat, double CenterAlt, 
    double RadiusMeters, int32 Level)
{
    // 估算经度半径 (在高纬度需要放大)
    double DegRadius = RadiusMeters / METERS_PER_DEGREE;
    double LatRad = FMath::DegreesToRadians(CenterLat);
    double CosLat = FMath::Abs(FMath::Cos(LatRad));
    // 避免除以0
    double LonRadius = (CosLat < 0.0001) ? DegRadius : (DegRadius / CosLat);

    FVector2D Start(CenterLon - LonRadius, CenterLat - DegRadius);
    FVector2D End(CenterLon + LonRadius, CenterLat + DegRadius);

    // 高度范围：中心高度 +/- 半径
    double MinH = CenterAlt - RadiusMeters;
    double MaxH = CenterAlt + RadiusMeters;

    return GenerateGridsByRange3D(Start, End, MinH, MaxH, Level);
}


int64 UGeoSOTUtils::GetGridCodeID(const FGeoSOTCell& Cell)
{
    // 简单的 ID 生成策略：高32位存Row，低32位存Col
    // 实际 GeoSOT 编码可能更复杂(Z-order)，但目前这个足够唯一标识
    return ((int64)Cell.Code.Row << 32) | (int64)Cell.Code.Col;
}

bool UGeoSOTUtils::CalculateGridWorldTransform(
    ACesiumGeoreference* GeoRef,
    const FGeoSOTCell& Cell,
    FVector& OutCenterWorld,
    FQuat& OutRotationWorld,
    FVector& OutScale)
{
    if (!GeoRef) return false;

    // 1. 计算中心点 (World)
    double CenterLon = (Cell.MinLon + Cell.MaxLon) * 0.5;
    double CenterLat = (Cell.MinLat + Cell.MaxLat) * 0.5;
    double CenterAlt = (Cell.MinHeight + Cell.MaxHeight) * 0.5;
    
    OutCenterWorld = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(CenterLon, CenterLat, CenterAlt));

    // 2. 计算方向向量 (World) - 局部切线系
    // 北向点
    FVector NorthPos = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(CenterLon, Cell.MaxLat, CenterAlt));
    // 东向点
    FVector EastPos  = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Cell.MaxLon, CenterLat, CenterAlt));
    // 天向点
    FVector UpPos    = GeoRef->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(CenterLon, CenterLat, Cell.MaxHeight));

    FVector DirNorth = (NorthPos - OutCenterWorld);
    FVector DirEast  = (EastPos - OutCenterWorld);
    FVector DirUp    = (UpPos - OutCenterWorld);

    // 3. 构建旋转 (World)
    // 使用 MakeFromXY 确保正交化，X指向北，Y指向东
    OutRotationWorld = FRotationMatrix::MakeFromXY(DirNorth, DirEast).ToQuat();

    // 4. 计算缩放 (物理尺寸 / 50.0f)
    // 标准 Cube 半径 50，所以用物理半长除以 50
    OutScale.X = DirNorth.Size() / 50.0f; 
    OutScale.Y = DirEast.Size() / 50.0f;   
    OutScale.Z = DirUp.Size() / 50.0f;

    return true;
}