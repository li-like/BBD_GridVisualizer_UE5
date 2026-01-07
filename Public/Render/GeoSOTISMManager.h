#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CesiumGeoreference.h"
#include "Cesium3DTileset.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Core/GeoSOTUtils.h"
#include "GeoSOTISMManager.generated.h"

// --- 单个网格的基础信息 ---
// 用于存储"疑似空闲"的网格数据，以便后续进行实时重检
struct FSimpleGridInfo
{
    int64 GridCode; // [新增] 数据库主键 / 唯一标识
    FTransform GridTransform; // 包含 Location(中心), Rotation(方向), Scale(大小)
    int32 InstanceStartIndex; // 在 FreeISM 中的实例索引
};

// --- Chunk 数据包 ---
// 一个大 Chunk 包含渲染组件和待检测列表
USTRUCT()
struct FChunkPackage
{
    GENERATED_BODY()

    UPROPERTY()
    UInstancedStaticMeshComponent* FreeISM = nullptr;      // 绿色组件

    UPROPERTY()
    UInstancedStaticMeshComponent* OccupiedISM = nullptr;  // 红色组件

    // 待重检列表：存储该 Chunk 内目前显示为"绿色"的网格
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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    ACesium3DTileset* TargetTileset;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UStaticMesh* BaseMesh; // 必须是 Engine/BasicShapes/Cube

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UMaterialInterface* SafeWireMaterial;     // 绿色线框材质实例
    
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Config")
    UMaterialInterface* ObstacleWireMaterial; // 红色线框材质实例

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

    // =================================================================
    // 实时检测配置 (近快远慢)
    // =================================================================

    // 高频检测范围 (厘米) - 默认 500m
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    double HighPriorityRadius = 50000.0;

    // 近处检测间隔 (秒) - 建议 0.1s
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    float NearCheckInterval = 0.1f;

    // 远处检测间隔 (秒) - 建议 1.0s
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|RealTime")
    float FarCheckInterval = 1.0f;

private:
    // =================================================================
    // 内部逻辑
    // =================================================================

    UPROPERTY()
    TMap<int64, FChunkPackage> ChunkMap;

    FVector LastUpdateLocation;
    bool bIsUpdatingChunks;
    
    // 计时器
    float NearTimer = 0.0f;
    float FarTimer = 0.0f;

    // 核心函数
    void UpdateChunks(const FVector& PlayerLocation);
    void CreateChunk(int64 ChunkKey, const FGeoSOTCell& ChunkCell);
    
    // 实时检测：bCheckFarRegions 为 true 时才检查远处网格
    void ProcessRealTimeDetection(const FVector& PlayerLocation, bool bCheckFarRegions);

    // 物理检测：支持 OBB 旋转
    bool CheckGridCollision(const FVector& Center, const FVector& BoxExtent, const FQuat& Rotation) const;

    FVector LlhToUnreal(double Lon, double Lat, double Alt);
};