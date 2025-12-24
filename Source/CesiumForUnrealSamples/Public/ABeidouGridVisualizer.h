#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CesiumGeoreference.h" 
#include "ABeidouGridVisualizer.generated.h"

UCLASS()
class CESIUMFORUNREALSAMPLES_API ABeidouGridVisualizer : public AActor
{
    GENERATED_BODY()

public:
    ABeidouGridVisualizer();

protected:
    virtual void BeginPlay() override;

public:
    virtual void Tick(float DeltaTime) override;

    // --- 【核心控制参数】 ---

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Beidou Grid | Core")
    ACesiumGeoreference* CesiumGeoreference; // Cesium 坐标转换依赖

    /**
     * @brief 当前要展示的网格层级。层级越高，网格越细。
     * 示例：Level 10 对应 0.001 度步长。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Core", meta = (ClampMin = "1", ClampMax = "15"))
    int32 CurrentGridLevel = 5;

    /**
     * @brief 网格的基准高度。决定网格底面的Z坐标（米）。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Core")
    double BaseAltitude = 0.0;

    /**
     * @brief 网格的垂直厚度（米）。决定体素的高度。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Core")
    double GridThickness = 50.0;

    // --- 【展示区域控制：改为 Min/Max 边界】 ---

/**
 * @brief 网格绘制区域的最小经度 (度)。
 */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MinLongitude = 144.950;

    /**
     * @brief 网格绘制区域的最大经度 (度)。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MaxLongitude = 144.960;

    /**
     * @brief 网格绘制区域的最小纬度 (度)。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MinLatitude = -37.810;

    /**
     * @brief 网格绘制区域的最大纬度 (度)。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MaxLatitude = -37.800;

    /**
     * @brief 网格绘制区域的最小高度 (米)。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MinAltitude = 400.0;

    /**
     * @brief 网格绘制区域的最大高度 (米)。
     */
    UPROPERTY(EditAnywhere, Category = "Beidou Grid | Display")
    double MaxAltitude = 500.0;

    // --- 【可视化设置】 ---
    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    float LineThickness = 5.0f;

    /**
     * @brief 用于保存经纬度步长的结构体。
     */
    struct FGridStep
    {
        double LonStep; // 经度步长 (度)
        double LatStep; // 纬度步长 (度)
    };

private:


    /**
     * @brief 核心函数：根据当前层级计算并绘制北斗网格。
     */
    void DrawBeidouGrid();

    /**
     * @brief 【新增】根据网格层级获取标准的**高度步长** (米)。
     * 遵循 GB/T 39409-2020 高度域不等距划分思想。
     * @return 返回该层级的高度步长，如果层级无效返回 0.0。
     */
    double GetAltitudeStepByLevel(int32 Level) const;

    /**
         * @brief 辅助函数：将索引 (0-14) 转换为 Base-15 字符 (0-9, A-E)。
         */
    FString IndexToCodeCharacter(int32 Index) const;

    /**
     * @brief 辅助函数：根据象限和方向要求，获取正确的 Base-15 字符。
     * @param Index 计算出的网格索引 (0-14)
     * @param bReverseDirection 是否需要反向编码 (如 E-0 顺序)
     * @return 编码字符 (0-9, A-E)
     */
    FString GetDirectionalCode(int32 Index, bool bReverseDirection) const;

    /**
     * @brief 【核心函数】根据地理坐标和层级，计算北斗三维网格码 (标准实现)。
     */
    FString GetGridCode(double Lon, double Lat, double Alt, int32 Level) const;

    /**
     * @brief 将经纬高转换为UE坐标 (复用 ACesiumVoxelGridGenerator 的逻辑)。
     */
    FVector GetUnrealLocation(double Longitude, double Latitude, double Height) const;

    /**
     * @brief 查找表：根据层级获取对应的经纬度步长 (度)。
     * @param Level 当前网格层级
     * @param OutLonStep 输出经度步长
     * @param OutLatStep 输出纬度步长
     * @return 如果层级有效则返回 true
     */
    bool GetGridStepByLevel(int32 Level, double& OutLonStep, double& OutLatStep) const;

    /**
     * @brief 检查指定体素体积内是否有物体占据 (复用占据检测逻辑)。
     */
    bool IsVoxelOccupied(const FVector& Center, const FVector& HalfExtents) const;
};