#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CesiumGeoreference.h"
#include "Cesium3DTileset.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Core/GeoSOTUtils.h"
#include "GeoSOTISMManager.generated.h"

// --- [新增] 单个网格的基础信息 ---
// ISM 组件只存 Transform，不存逻辑数据。
// 为了能"回头"重新检测这个网格是否撞到了新加载的楼，我们需要存下它的位置信息。
struct FSimpleGridInfo
{
    FVector Center;      // 网格中心 (Unreal 坐标)
    FVector Extent;      // 网格半长宽
    int32 InstanceStartIndex; // 在 ISM 中的起始索引 (如果是线框模式，可能对应12个实例；如果是Cube模式对应1个)
};

// --- [修改] Chunk 数据包 ---
// 不仅包含渲染组件，还包含需要持续监测的数据列表
USTRUCT()
struct FChunkPackage
{
    GENERATED_BODY()

    UPROPERTY()
    UInstancedStaticMeshComponent* FreeISM = nullptr;      // 绿色组件 (渲染空闲区域)

    UPROPERTY()
    UInstancedStaticMeshComponent* OccupiedISM = nullptr;  // 红色组件 (渲染障碍物)

    // [新增] 待重检列表
    // 存储该 Chunk 内所有目前判定为"绿色"的网格。
    // 在 Tick 中，我们会遍历这个列表，再次进行物理检测。
    // 如果检测到了障碍物，将其从 FreeISM 移除并添加到 OccupiedISM。
    TArray<FSimpleGridInfo> SuspectedFreeGrids;
};

UCLASS()
class GEOSOTSYSTEM_API AGeoSOTISMManager : public AActor
{
    GENERATED_BODY()
    
public:    
    AGeoSOTISMManager();

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;

public:
    // =================================================================
    // 外部引用与资源配置
    // =================================================================
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    ACesiumGeoreference* Georeference;

    // [新增] 目标 Cesium Tileset，用于确保物理检测针对正确的对象
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    ACesium3DTileset* TargetTileset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UStaticMesh* BaseMesh;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UMaterialInterface* SafeWireMaterial; // 绿色 (Free)
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UMaterialInterface* ObstacleWireMaterial; // 红色 (Occupied)

    // =================================================================
    // 核心参数设置
    // =================================================================
    
    // 视野半径 (米)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Setting")
    double ViewRadius = 3000.0;

    // 渲染层级 (建议：L19=100m级 或 L20=50m级)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Setting")
    int32 RenderLevel = 19; 

    // 分块层级 (用于管理加载范围)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Setting")
    int32 ChunkLevel = 10;
    

    // 垂直渲染范围限制 (米)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Setting")
    double LowAltitudeCeiling = 2000.0;
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Setting")
    double MinAltitude = 10.0;

    // [新增] 高频检测范围 (厘米)
    //在这个范围内，我们会非常频繁地检测障碍物 (例如 50000 = 500米)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    double HighPriorityRadius = 50000.0;

    // [新增] 近处检测间隔 (秒) - 建议 0.1 或 0.05 (极快)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    float NearCheckInterval = 0.1f;

    // [新增] 远处检测间隔 (秒) - 建议 1.0 或 2.0 (省性能)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    float FarCheckInterval = 1.0f;

private:
    // =================================================================
    // 内部逻辑与数据
    // =================================================================

    // 核心 Map: Key = ChunkID, Value = 组件+数据
    UPROPERTY()
    TMap<int64, FChunkPackage> ChunkMap;

    // 状态追踪
    FVector LastUpdateLocation;
    bool bIsUpdatingChunks; // 是否正在生成新的 Chunk (避免重复触发)
    // [修改] 计时器拆分为两个
    float NearTimer = 0.0f;
    float FarTimer = 0.0f;

    // =================================================================
    // 核心函数声明
    // =================================================================

    // 1. 空间管理：根据玩家位置加载/卸载 Chunk
    void UpdateChunks(const FVector& PlayerLocation);

    // 2. 初始生成：异步生成网格并进行第一次检测
    void CreateChunk(int64 ChunkKey, const FGeoSOTCell& ChunkCell);
    
    // 3. [新增] 实时检测逻辑：遍历现有 Chunk，检查 Cesium 是否加载出了新建筑
    void ProcessRealTimeDetection(const FVector& PlayerLocation,bool bCheckFarRegions);

    // 工具：物理检测
    // 返回 true 表示有障碍物
    bool CheckGridCollision(const FVector& Center, const FVector& Extent) const;

    // 工具：坐标转换
    FVector LlhToUnreal(double Lon, double Lat, double Alt);


};