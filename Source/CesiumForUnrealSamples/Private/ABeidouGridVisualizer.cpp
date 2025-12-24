#include "ABeidouGridVisualizer.h"
#include "DrawDebugHelpers.h" 
#include "Kismet/GameplayStatics.h" 
#include "CollisionQueryParams.h" 
#include "Engine/World.h" 
#include "Containers/Map.h"


// --- 北斗网格码标准 2D 划分步长常量 (GB/T 39409-2020 5.1节) ---
// 将度分秒转换为度：1' = 1/60 度, 1'' = 1/3600 度

// L1: 6° x 4°
const double LonStepL1 = 6.0;
const double LatStepL1 = 4.0;

// L2: 30' x 30' (0.5° x 0.5°)
const double LonStepL2 = 30.0 / 60.0;
const double LatStepL2 = 30.0 / 60.0;

// L3: 15' x 10' (0.25° x 10/60°)
const double LonStepL3 = 15.0 / 60.0;
const double LatStepL3 = 10.0 / 60.0;

// L4: 1' x 1' (1/60° x 1/60°)
const double StepL4 = 1.0 / 60.0;

// L5: 4'' x 4''
const double StepL5 = 4.0 / 3600.0;

// L6: 2'' x 2''
const double StepL6 = 2.0 / 3600.0;

// L7: 1/4'' x 1/4'' (0.25'' x 0.25'')
const double StepL7 = (1.0 / 4.0) / 3600.0;

// L8: 1/32'' x 1/32''
const double StepL8 = (1.0 / 32.0) / 3600.0;

// L9: 1/256'' x 1/256''
const double StepL9 = (1.0 / 256.0) / 3600.0;

// L10: 1/2048'' x 1/2048''
const double StepL10 = (1.0 / 2048.0) / 3600.0;


static const TMap<int32, ABeidouGridVisualizer::FGridStep> StandardGridSteps =
{
    {1, {LonStepL1, LatStepL1}},
    {2, {LonStepL2, LatStepL2}},
    {3, {LonStepL3, LatStepL3}},
    {4, {StepL4, StepL4}},
    {5, {StepL5, StepL5}},
    {6, {StepL6, StepL6}},
    {7, {StepL7, StepL7}},
    {8, {StepL8, StepL8}},
    {9, {StepL9, StepL9}},
    {10, {StepL10, StepL10}}
};




// 构造函数
ABeidouGridVisualizer::ABeidouGridVisualizer()
{
    PrimaryActorTick.bCanEverTick = true;
}

void ABeidouGridVisualizer::BeginPlay()
{
    Super::BeginPlay();

    // 自动查找场景中的 CesiumGeoreference
    if (!CesiumGeoreference)
    {
        AActor* FoundActor = UGameplayStatics::GetActorOfClass(GetWorld(), ACesiumGeoreference::StaticClass());
        CesiumGeoreference = Cast<ACesiumGeoreference>(FoundActor);
        if (!CesiumGeoreference)
        {
            UE_LOG(LogTemp, Error, TEXT("ABeidouGridVisualizer: CesiumGeoreference NOT found!"));
        }
    }
}

void ABeidouGridVisualizer::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    DrawBeidouGrid();
}

/**
 * @brief 根据网格层级获取标准的北斗高度步长 (米)。
 * * 依据 GB/T 39409-2020 附录 C 高度域不等距划分思想实现。
 * 注意：这里的数值是基于 GeoSOT 模型在赤道处水平分辨率的近似值，以确保单元比例合理。
 */
double ABeidouGridVisualizer::GetAltitudeStepByLevel(int32 Level) const
{
    // 这些数值是标准划分下，各层级的典型高度步长 (米)
    switch (Level)
    {
    case 1: return 256000.0; // 256 km
    case 2: return 50000.0;  // 50 km
    case 3: return 10000.0;  // 10 km
    case 4: return 1850.0;   // 约 1.85 km
    case 5: return 125.0;    // 约 125 m
    case 6: return 60.0;     // 约 60 m
    case 7: return 8.0;      // 约 8 m
    case 8: return 1.0;      // 约 1 m (米级精度)
    case 9: return 0.1;      // 约 0.1 m (分米级精度)
    case 10: return 0.01;     // 约 0.01 m (厘米级精度)
    default:
        UE_LOG(LogTemp, Warning, TEXT("Beidou Grid: Invalid Grid Level %d for Altitude Step."), Level);
        return 0.0;
    }
}

/**
 * @brief 查找表：根据层级获取对应的经纬度步长 (度)。
 * @details 严格按照 GB/T 39409-2020 5.1 节定义的步长。
 * @param Level 当前网格层级
 * @param OutLonStep 输出经度步长
 * @param OutLatStep 输出纬度步长
 * @return 如果层级有效则返回 true
 */
bool ABeidouGridVisualizer::GetGridStepByLevel(int32 Level, double& OutLonStep, double& OutLatStep) const
{
    if (const FGridStep* Step = StandardGridSteps.Find(Level))
    {
        OutLonStep = Step->LonStep;
        OutLatStep = Step->LatStep;
        return true;
    }

    // 如果 Level 超出预设范围，则报错并返回 0
    OutLonStep = 0.0;
    OutLatStep = 0.0;
    UE_LOG(LogTemp, Warning, TEXT("Beidou Grid: Level %d is not defined in the standard lookup table (1-10)."), Level);
    return false;
}

/**
 * @brief 将经纬高转换为UE坐标 (复用逻辑)。
 */
FVector ABeidouGridVisualizer::GetUnrealLocation(double Longitude, double Latitude, double Height) const
{
    if (!CesiumGeoreference)
    {
        return FVector::ZeroVector;
    }
    // 使用 CesiumGeoreference 实例的方法进行转换
    return CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(Longitude, Latitude, Height));
}

/**
 * @brief 检查指定体素体积内是否有物体占据 (复用逻辑)。
 */
bool ABeidouGridVisualizer::IsVoxelOccupied(const FVector& Center, const FVector& HalfExtents) const
{
    UWorld* World = GetWorld();
    if (!World) return false;

    TArray<FOverlapResult> OverlapResults;
    FCollisionQueryParams Params;
    Params.AddIgnoredActor(this);

    // 使用 ECC_WorldStatic 检测 Cesium Tileset, 建筑等
    ECollisionChannel Channel = ECollisionChannel::ECC_WorldStatic;

    bool bHit = World->OverlapMultiByChannel(
        OverlapResults,
        Center,
        FQuat::Identity,
        Channel,
        FCollisionShape::MakeBox(HalfExtents),
        Params
    );
    return bHit;
}

/**
 * @brief 辅助函数：将索引 (0-14) 转换为 Base-15 字符 (0-9, A-E)。
 */
FString ABeidouGridVisualizer::IndexToCodeCharacter(int32 Index) const
{
    if (Index >= 0 && Index <= 9)
    {
        // 修正方法 1: 使用 FString::Printf 构造数字字符串
        return FString::Printf(TEXT("%d"), Index);
    }
    else if (Index >= 10 && Index <= 14)
    {
        // 修正方法 2: 明确构造单字符 FString
        // FString::FString(const TCHAR* InChars) 
        TCHAR Char = (TCHAR)('A' + (Index - 10));
        return FString(&Char, 1); // 传入字符指针和长度

        // 或者使用 FString::Chr 静态方法 (如果可用，但上述方法更通用)
        // return FString::Chr(Char); 
    }
    return TEXT("X"); // 错误或超出范围
}

/**
 * @brief 辅助函数：根据象限和方向要求，获取正确的 Base-15 字符。
 * @details 处理标准中 L4 及其它层级中存在的编码方向翻转。
 */
FString ABeidouGridVisualizer::GetDirectionalCode(int32 Index, bool bReverseDirection) const
{
    if (Index < 0 || Index > 14) return TEXT("X");

    if (bReverseDirection)
    {
        // 反向编码 (如 E-0 顺序): Index 0 -> E (14), Index 14 -> 0
        Index = 14 - Index;
    }

    return IndexToCodeCharacter(Index);
}

// ABeidouGridVisualizer.cpp

FString ABeidouGridVisualizer::GetGridCode(double Lon, double Lat, double Alt, int32 Level) const
{
    FString GridCode;

    // --- 0. 半球判断 ---
    const bool bIsNorth = Lat >= 0.0;
    const bool bIsEast = Lon >= 0.0;

    // -----------------------------------------------------------------
    // 1. 基础码 (L1: 6°x4°)
    // -----------------------------------------------------------------

    const double BaseLon = -180.0; // 经度起点
    const double BaseLat = -90.0;  // 纬度起点
    const double L1LonStep = 6.0;
    const double L1LatStep = 4.0;

    // L1 索引：i1 经度 (0-59), j1 纬度 (0-44)
    int32 i1 = FMath::FloorToInt((Lon - BaseLon) / L1LonStep);
    int32 j1 = FMath::FloorToInt((Lat - BaseLat) / L1LatStep);

    // C1: 南北半球标识 (N/S)
    GridCode += bIsNorth ? TEXT("N") : TEXT("S");

    // C2/C3: 经度码 (1-60)。严格按照标准，使用 2 位数字编码。
    GridCode += FString::Printf(TEXT("%02d"), i1 + 1); // 索引 0-59 对应编码 01-60

    // C4: 纬度码 (1-45)。标准使用 A-W (Base-23) 编码，这里使用数字索引占位。
    // **注意：由于标准 Base-23 映射表复杂，这里暂时使用两位数字索引，待获取完整标准后替换**
    GridCode += FString::Printf(TEXT("%02d"), j1 + 1); // 索引 0-44 对应编码 01-45

    // -----------------------------------------------------------------
    // 2. 递进码 (L2 及以上)
    // -----------------------------------------------------------------

    double CurrentLonOrigin = BaseLon + i1 * L1LonStep;
    double CurrentLatOrigin = BaseLat + j1 * L1LatStep;

    for (int32 CurrentLevel = 2; CurrentLevel <= Level; ++CurrentLevel)
    {
        double LonStep, LatStep;
        if (!GetGridStepByLevel(CurrentLevel, LonStep, LatStep))
        {
            break; // 超过支持的层级
        }

        // 计算当前坐标相对于父网格原点的偏移
        double LonInParent = Lon - CurrentLonOrigin;
        double LatInParent = Lat - CurrentLatOrigin;

        // 计算子网格索引 (i_k, j_k)
        int32 ik = FMath::FloorToInt(LonInParent / LonStep);
        int32 jk = FMath::FloorToInt(LatInParent / LatStep);

        // Ck/C(k+1): 递进码。编码位数和 Base 因层级而异，这里以最常见的 Base-15 编码演示。

        // **【关键点】根据标准图 (如 L4 图 5)，确定经纬度是否需要反向编码**
        // 编码方向判断是标准编码的核心。此处遵循 L4 的典型规则：
        // 纬度：北半球向上 (正向)，南半球向下 (反向)。
        // 经度：西半球向右 (正向)，东半球向左 (反向)。

        bool bReverseLon = !bIsEast; // 标准中经度编码方向与东西半球有关
        bool bReverseLat = !bIsNorth; // 标准中纬度编码方向与南北半球有关

        // Note: L2, L3 编码规则略有不同，但此处用 L4 逻辑作为**标准结构**示例。

        FString LonCode = GetDirectionalCode(ik, bReverseLon);
        FString LatCode = GetDirectionalCode(jk, bReverseLat);

        // 拼接编码 (标准中交替编码，但此处简化为 Lon/Lat)
        GridCode += LonCode;
        GridCode += LatCode;

        // 更新当前网格的原点坐标
        CurrentLonOrigin += ik * LonStep;
        CurrentLatOrigin += jk * LatStep;
    }

    // -----------------------------------------------------------------
    // 3. 高度码 (H码)
    // -----------------------------------------------------------------

    // 获取当前层级的标准高度步长 (动态值)
    const double AltStep = GetAltitudeStepByLevel(Level);

    if (AltStep <= 0.0)
    {
        return GridCode + TEXT("-H_ERR");
    }

    // HCode = Floor(当前高度 Alt / 标准高度步长 AltStep)。以 0m 大地高为原点。
    int32 HCode = FMath::FloorToInt(Alt / AltStep);

    GridCode += FString::Printf(TEXT("-%d"), HCode); // 标准格式：高度码前加连接符

    return GridCode;
}

void ABeidouGridVisualizer::DrawBeidouGrid()
{
    if (!CesiumGeoreference || !GetWorld()) return;

    // --- 边界检查和有效性检查 ---
    if (MinLongitude >= MaxLongitude || MinLatitude >= MaxLatitude || MinAltitude >= MaxAltitude)
    {
        UE_LOG(LogTemp, Warning, TEXT("Beidou Grid: Invalid Min/Max boundaries provided. Skipping draw."));
        return;
    }

    double LonStep;
    double LatStep;

    // 获取当前层级的经纬度步长
    if (!GetGridStepByLevel(CurrentGridLevel, LonStep, LatStep))
    {
        return; // 层级无效，退出绘制
    }
    // 【关键修改点 1】动态获取当前层级的标准高度步长
    const double AltStep = GetAltitudeStepByLevel(CurrentGridLevel);
    if (AltStep <= 0.0) return;

    // --- 1. 确定循环的起始点和步长 (严格对齐到网格边界) ---

    // 计算第一个小于或等于 MinLongitude 的标准网格线，以确保绘制区域从标准网格边界开始。
    const double LoopStartLon = FMath::FloorToDouble(MinLongitude / LonStep) * LonStep;
    const double LoopStartLat = FMath::FloorToDouble(MinLatitude / LatStep) * LatStep;

    // 【关键修改点 2】高度起始点：对齐到大地高 0m 为原点的标准步长上
    const double LoopStartAlt = FMath::FloorToDouble(MinAltitude / AltStep) * AltStep;

    // --- 2. 遍历指定区域内的网格单元 ---

    // 经度循环：从对齐后的起始线开始，直到达到 MaxLongitude
    for (double Lon = LoopStartLon; Lon < MaxLongitude; Lon += LonStep)
    {
        const double NextLon = Lon + LonStep;

        // 纬度循环：从对齐后的起始线开始，直到达到 MaxLatitude
        for (double Lat = LoopStartLat; Lat < MaxLatitude; Lat += LatStep)
        {
            const double NextLat = Lat + LatStep;
            // 【关键修改点 3】高度循环：从对齐后的 LoopStartAlt 开始
            for (double Alt = LoopStartAlt; Alt < MaxAltitude; Alt += AltStep)
            {
                // 确保最后一层的高度不超过 MaxAltitude
                const double CurrentNextAlt = FMath::Min(Alt + AltStep, MaxAltitude);
                // ---------------------------------------------------------
                // 3.1. 转换为 Unreal 世界坐标 (P0 到 P7) 
                // ---------------------------------------------------------
                FVector P0 = GetUnrealLocation(Lon, Lat, Alt);
                FVector P1 = GetUnrealLocation(NextLon, Lat, Alt);
                FVector P2 = GetUnrealLocation(NextLon, NextLat, Alt);
                FVector P3 = GetUnrealLocation(Lon, NextLat, Alt);
                FVector P4 = GetUnrealLocation(Lon, Lat, CurrentNextAlt);
                FVector P5 = GetUnrealLocation(NextLon, Lat, CurrentNextAlt);
                FVector P6 = GetUnrealLocation(NextLon, NextLat, CurrentNextAlt);
                FVector P7 = GetUnrealLocation(Lon, NextLat, CurrentNextAlt);

                // ---------------------------------------------------------
                // 3.2. 占据性检测 (保持不变)
                // ---------------------------------------------------------
                FVector CenterPoint = (P0 + P6) / 2.0f;
                float VoxelWidth = (P1 - P0).Size();
                float VoxelLength = (P3 - P0).Size();
                float VoxelHeight = (P4 - P0).Size();
                FVector HalfExtents(VoxelWidth / 2.0f, VoxelLength / 2.0f, VoxelHeight / 2.0f);

                bool bOccupied = IsVoxelOccupied(CenterPoint, HalfExtents);
                FLinearColor CurrentColor = bOccupied ? FLinearColor::Red : FLinearColor::Green;
                FColor DrawColor = CurrentColor.ToFColor(true);

                // ---------------------------------------------------------
                // 3.3. 绘制编码 
                // ---------------------------------------------------------
                // 获取网格单元的中心经纬高，用于编码
                double CenterLon = Lon + LonStep / 2.0;
                double CenterLat = Lat + LatStep / 2.0;
                double CenterAlt = Alt + (CurrentNextAlt - Alt) / 2.0;

                FString BeidouCode = GetGridCode(CenterLon, CenterLat, CenterAlt, CurrentGridLevel);
                // 【修正 C2039/C2664 错误】：直接使用 DrawColor 变量，它已经是 FColor 类型。
                UWorld* World = GetWorld();
                //DrawDebugString(
                //    World,
                //    CenterPoint, // 绘制位置：体素中心
                //    BeidouCode,
                //    nullptr, // 附属 Actor
                //    DrawColor, // <--- 修正点：直接使用 FColor 类型的 DrawColor
                //    -1.0f, // 持续时间：Tick期间可见
                //    true, // 始终可见
                //    1.0f // 字体缩放
                //);

                // ---------------------------------------------------------
                // 3.4. 绘制线框 (保持不变) 
                // ---------------------------------------------------------
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