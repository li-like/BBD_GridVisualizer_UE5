// Copyright 2020-2021 CesiumGS, Inc. and Contributors


#include "Fission/GeoSOTRiskEnvironment.h"
#include "Math/UnrealMathUtility.h"

UGeoSOTRiskEnvironment::UGeoSOTRiskEnvironment()
{
}

void UGeoSOTRiskEnvironment::InitializeMicroEnvironment(double MinLat, double MaxLat, double MinLon, double MaxLon)
{
    BaseStations.Empty();

    // 在这 0.01 度 (约 1km x 1km) 的微小空域内，我们在对角线部署两个“微基站”
    // 这样人为制造出：双基站覆盖区(安全)、单基站边缘区(弱风险)、两头不靠的盲区(高风险)

    // 基站 A: 放在左下角 20% 处
    FVirtualBaseStation StationA;
    StationA.Lat = MinLat + (MaxLat - MinLat) * 0.2;
    StationA.Lon = MinLon + (MaxLon - MinLon) * 0.2;
    StationA.Alt = 50.0; // 架设在 50 米高度
    StationA.MaxRangeMeters = 600.0; // 覆盖半径 600 米
    BaseStations.Add(StationA);

    // 基站 B: 放在右上角 80% 处
    FVirtualBaseStation StationB;
    StationB.Lat = MinLat + (MaxLat - MinLat) * 0.8;
    StationB.Lon = MinLon + (MaxLon - MinLon) * 0.8;
    StationB.Alt = 80.0; // 架设在 80 米高度
    StationB.MaxRangeMeters = 550.0; // 覆盖半径 550 米
    BaseStations.Add(StationB);
}

double UGeoSOTRiskEnvironment::GetWeatherRisk(double Lat, double Lon, double Alt, float TimeSeconds) const
{
    // 【核心微观尺度调优：噪声频率缩放】
    // 0.01 度的经纬度变化在常规柏林噪声中几乎是一条平线（没有区分度）。
    // 将其乘以 500.0，相当于把 0.01 度的空间拉伸跨越了 5.0 的噪声周期。
    // 这会在 1 平方公里的空域内产生 2-3 个明显的“雷暴风眼”和“晴朗区”。
    double NoiseScale = 500.0; 
    
    // 【核心动态调优：演化速度】
    // 将时间流逝速度放慢，使得风暴像真实的云层一样缓慢平移演化，而不是像闪电一样狂闪。
    double TimeScale = 0.2; 

    // 使用 UE5 内置的 3D 柏林噪声。时间作为 Z 轴，产生 4D 演化效果。
    // 高度 Alt 目前除以 200.0，使气象场在垂直方向上也有缓慢分层。
    float RawNoise = FMath::PerlinNoise3D(FVector(
        Lat * NoiseScale, 
        Lon * NoiseScale, 
        (Alt / 200.0) + (TimeSeconds * TimeScale) 
    ));

    // RawNoise 的返回值在 [-1.0, 1.0] 之间。
    // 1. 将其映射到 [0.0, 1.0]
    double NormalizedRisk = (RawNoise + 1.0) * 0.5;

    // 2. 使用 Pow(x, 2) 增加对比度：让普通区域天气保持良好 (分数低)，
    // 而极端雷暴仅仅聚集在中心小部分区域，突出“风险场”的尖锐特征。
    NormalizedRisk = FMath::Pow(NormalizedRisk, 2.0); 

    // 确保返回值被严格截断在论文规定的归一化边界内
    return FMath::Clamp(NormalizedRisk, 0.0, 1.0);
}

double UGeoSOTRiskEnvironment::GetCommunicationRisk(double Lat, double Lon, double Alt) const
{
    if (BaseStations.Num() == 0) return 1.0; // 若未部署基站，则默认全局失联（风险 1.0）

    double MinRisk = 1.0; // 初始假设处于绝对盲区

    // 遍历所有基站，寻找信号最强（风险最小）的接入点
    for (const FVirtualBaseStation& Station : BaseStations)
    {
        double DistMeters = CalculateDistanceMeters(Lat, Lon, Station.Lat, Station.Lon, Alt, Station.Alt);
        
        // 信号衰减模型核心逻辑：距离越近风险越小
        double StationRisk = DistMeters / Station.MaxRangeMeters;
        
        // 模拟自由空间损耗 (Free Space Path Loss, 与距离平方成正比)
        // 这会让靠近基站时信号极好，到了边缘处信号衰减骤然加快
        StationRisk = FMath::Pow(StationRisk, 2.0);
        
        StationRisk = FMath::Clamp(StationRisk, 0.0, 1.0);

        // 我们无人机会自动连接信号最好的基站
        if (StationRisk < MinRisk)
        {
            MinRisk = StationRisk;
        }
    }

    return MinRisk;
}

double UGeoSOTRiskEnvironment::CalculateDistanceMeters(double Lat1, double Lon1, double Lat2, double Lon2, double Alt1, double Alt2) const
{
    // 近似球面距离转换：1度经纬度大约 111320 米。
    // 对于 0.01 度的微观环境，不需要复杂的 Haversine 公式，这种泰勒展开近似已经具备足够的科学精度。
    double dLat = (Lat2 - Lat1) * 111320.0;
    // 经度的物理跨度受当前纬度的余弦值影响
    double dLon = (Lon2 - Lon1) * 111320.0 * FMath::Cos(FMath::DegreesToRadians(Lat1));
    double dAlt = Alt2 - Alt1;

    // 空间欧氏距离
    return FMath::Sqrt(dLat*dLat + dLon*dLon + dAlt*dAlt);
}