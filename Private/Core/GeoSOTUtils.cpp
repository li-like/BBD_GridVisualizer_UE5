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


int64 UGeoSOTUtils::GenerateGeoSOTCode(int32 Row, int32 Col, int32 Level, int32 AltIndex)
{
    // 1. 计算 2D Morton 码 (经纬度)
    // Row 放高位 (奇数位)，Col 放低位 (偶数位)
    uint64 SpacedRow = SpreadBits((uint32)Row);
    uint64 SpacedCol = SpreadBits((uint32)Col);
    
    // 组合成 2D 编码 (通常占用约 2*Level 位，例如 L21 占用 42位)
    int64 MortonCode2D = (int64)((SpacedRow << 1) | SpacedCol);
    
    // 2. 融合高度信息
    // 策略：将 2D 编码左移 16 位，腾出低 16 位给高度
    // 限制：AltIndex 不能超过 65535 (0xFFFF)
    // 限制：2D 编码不能超过 47 位 (Level 23 以内都安全)
    
    int64 FinalCode = (MortonCode2D << 16) | (AltIndex & 0xFFFF);
    
    return FinalCode;
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


// 辅助函数：将 32位整数的位扩展开 (0000abc -> 0a0b0c)
// 用于构造 Morton Code
uint64 UGeoSOTUtils::SpreadBits(uint32 x)
{
    uint64 n = x;
    n = (n | (n << 32)) & 0x00000000FFFFFFFF;
    n = (n | (n << 16)) & 0x0000FFFF0000FFFF;
    n = (n | (n << 8))  & 0x00FF00FF00FF00FF;
    n = (n | (n << 4))  & 0x0F0F0F0F0F0F0F0F;
    n = (n | (n << 2))  & 0x3333333333333333;
    n = (n | (n << 1))  & 0x5555555555555555;
    return n;
}

// 辅助函数：逆运算
// 辅助函数：位收缩 (Inverse Morton Code)
uint32 UGeoSOTUtils::CompactBits(uint64 x)
{
    // 只保留奇数位
    uint64 n = x & 0x5555555555555555;
    
    // 开始压缩 (这是标准的 Inverse Morton 算法)
    // 注意：必须严格按照 1, 2, 4, 8, 16 的顺序进行位移动
    
    n = (n ^ (n >> 1))  & 0x3333333333333333;
    n = (n ^ (n >> 2))  & 0x0F0F0F0F0F0F0F0F;
    n = (n ^ (n >> 4))  & 0x00FF00FF00FF00FF; // <--- 之前可能漏了这一步或写错了！
    n = (n ^ (n >> 8))  & 0x0000FFFF0000FFFF;
    n = (n ^ (n >> 16)) & 0x00000000FFFFFFFF;
    
    return (uint32)n;
}

void UGeoSOTUtils::DecodeGeoSOTCode(int64 Code, int32& OutRow, int32& OutCol, int32& OutLevel, int32& OutAltLayer)
{
    // 1. 提取高度 (低16位)
    OutAltLayer = (int32)(Code & 0xFFFF);

    // 2. 提取 2D Morton 码 (高位)
    int64 Morton2D = Code >> 16;

    // 3. 分离 Row 和 Col
    // Morton = (Row << 1) | Col
    // Row 在奇数位，Col 在偶数位
    
    // 提取 Col (偶数位)
    OutCol = CompactBits(Morton2D);
    
    // 提取 Row (奇数位 -> 右移1位变成偶数位 -> Compact)
    OutRow = CompactBits(Morton2D >> 1);

    // Level 通常不存就在 Code 里 (除非你以前存了)，这里假设由外部传入或固定
    // 如果你之前的 Generate 把 Level 存在了更高位，这里也要相应提取
    // 目前我们先透传
    OutLevel = 21; // 默认或者从参数获取
}

void UGeoSOTUtils::GetNeighbors3D(int64 CurrentCode, int32 Level, TArray<int64>& OutNeighbors)
{
    int32 R, C, L, Alt;
    DecodeGeoSOTCode(CurrentCode, R, C, L, Alt);

    // 6 邻域偏移
    int32 Offsets[6][3] = {
        {1,0,0}, {-1,0,0}, // 北, 南 (Row变化)
        {0,1,0}, {0,-1,0}, // 东, 西 (Col变化)
        {0,0,1}, {0,0,-1}  // 上, 下 (Alt变化)
    };

    for(int i=0; i<6; i++)
    {
        int32 NewR = R + Offsets[i][0];
        int32 NewC = C + Offsets[i][1];
        int32 NewAlt = Alt + Offsets[i][2];

        // 简单的边界检查 (防止 Alt < 0)
        if (NewAlt < 0) continue; 

        // 重新编码
        OutNeighbors.Add(GenerateGeoSOTCode(NewR, NewC, Level, NewAlt));
    }
}

void UGeoSOTUtils::GetGeoSOTCenter(int32 Row, int32 Col, int32 Level, double& OutLat, double& OutLon)
{
    // GeoSOT 定义：地球空间被扩展为 512° x 512°
    // 逻辑区间为 [-256, 256]
    // 每一格的度数跨度 (Step) = 512 / 2^Level
    
    // 注意：1LL << Level 是为了防止 Level 较大时 int32 溢出，虽然 L21 不会溢出，但好习惯很重要
    double Step = 512.0 / (double)(1LL << Level); 

    // 计算中心点公式：
    // Min = Index * Step - 256
    // Center = Min + Step/2 = (Index + 0.5) * Step - 256
    
    OutLat = ((double)Row + 0.5) * Step - 256.0;
    OutLon = ((double)Col + 0.5) * Step - 256.0;
}

void UGeoSOTUtils::LatLonToGeoSOT(double Lat, double Lon, int32 Level, int32& OutRow, int32& OutCol)
{
    // 1. 将经纬度从 [-256, 256] 映射到 [0, 512] 的正数空间
    // 真实的经纬度范围是 Lat[-90, 90], Lon[-180, 180]
    // 但 GeoSOT 算法统一加 256 处理
    double OffsetLat = Lat + 256.0;
    double OffsetLon = Lon + 256.0;

    // 2. 计算当前层级的步长 (网格分辨率)
    double Step = 512.0 / (double)(1LL << Level);

    // 3. 计算索引 (向下取整)
    OutRow = (int32)(OffsetLat / Step);
    OutCol = (int32)(OffsetLon / Step);
}