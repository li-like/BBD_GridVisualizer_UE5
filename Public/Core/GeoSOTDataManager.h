#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Containers/Queue.h"
#include "GeoSOTDataManager.generated.h"

// 前置声明
class UWorld;
class ACesiumGeoreference;
class UGeoSOTRiskEnvironment;

/** 节点状态枚举 */
UENUM(BlueprintType)
enum class EChunkState : uint8
{
    Unloaded,
    Loading,
    Ready
};

/** 单个网格节点的数据结构 */
USTRUCT(BlueprintType)
struct FGridNodeData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    int32 Level;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double CenterLat;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double CenterLon;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double CenterAlt;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FString GeoSOTCode;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Rho_Risk; // 物理冲突风险

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double W_Risk;   // 气象风险

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Q_Risk;   // 通信风险

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double Score;    // 综合安全评分

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    bool bIsObstacle;
};

/** 区块数据结构 */
USTRUCT(BlueprintType)
struct FGridChunk
{
    GENERATED_BODY()

    UPROPERTY()
    FString ChunkKey;

    UPROPERTY()
    EChunkState State;

    UPROPERTY()
    int32 ChunkX;

    UPROPERTY()
    int32 ChunkY;

    UPROPERTY()
    TArray<FGridNodeData> Nodes;

    UPROPERTY()
    bool bIsTopologyFinalized = false; // 拓扑是否锁死

    UPROPERTY()
    float LastPhysicsScanTime = -999.0f;
};

UCLASS(BlueprintType)
class GEOSOTSYSTEM_API UGeoSOTDataManager : public UObject
{
    GENERATED_BODY()

public:
    UGeoSOTDataManager();

    /** 初始化管理器 */
    void InitializeManager(UWorld* InWorld, ACesiumGeoreference* InGeoRef, UGeoSOTRiskEnvironment* InRiskEnv,
                          double InMinLat, double InMaxLat, double InMinLon, double InMaxLon, 
                          double InMinAlt, double InMaxAlt, int32 InTargetLevel);

    /** 更新观测者（相机）位置，触发区块加载 */
    void UpdateObserverPosition(const FVector& CameraPos);

    /** 处理异步加载队列 */
    void ProcessLoadQueue();

    /** 滚动动态扫描（核心 3：解决 Cesium 异步物理判定的关键） */
    void RollingDynamicScan(float CurrentTimeSeconds);

    /** 获取半径内的就绪节点 */
    void GetReadyNodesInRadius(const FVector& CameraPos, double Radius, TArray<FGridNodeData>& OutNodes) const;

public:
    // --- 算法参数 ---
    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    bool bEnableAdaptiveFission = true;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    int32 MaxFissionLevel = 23;

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Weights")
    double Weight_Rho = 0.6;   // 物理风险权重

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Weights")
    double Weight_Weather = 0.2; // 气象风险权重

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Weights")
    double Weight_Comm = 0.2;    // 通信风险权重

    UPROPERTY(EditAnywhere, Category = "GeoSOT|Settings")
    double Threshold_Theta = 0.6; // 安全阈值

private:
    /** 构建区块内的节点（初始 L20） */
    void BuildChunkNodes(FGridChunk& OutChunk);

    /** 递归评估并裂变 */
    void EvaluateAndFission(FGridChunk& OutChunk, uint32 LatIdx, uint32 LonIdx, int32 AltIdx, int32 CurrentLevel);

    /** 辅助：子网格细分测试 */
    void SubdivideTestNode(FGridChunk& Chunk, uint32 PLatIdx, uint32 PLonIdx, int32 PAltIdx, int32 PLevel, int32 TLevel);

private:
    UPROPERTY()
    UWorld* WorldPtr;

    UPROPERTY()
    ACesiumGeoreference* GeoRefPtr;

    UPROPERTY()
    UGeoSOTRiskEnvironment* RiskEnvPtr;

    TMap<FString, FGridChunk> GlobalChunkMap;
    TQueue<FString> ChunkLoadQueue;
    TArray<FString> ReadyChunkKeys;

    int32 TargetLevel = 20;
    int32 ChunkGridSize = 16; // 默认每个 Chunk 16x16 单元格

    // 边界索引
    uint32 GlobalMinLatIdx, GlobalMaxLatIdx, GlobalMinLonIdx, GlobalMaxLonIdx;
    double MinAlt, MaxAlt;

    int32 CurrentScanChunkIndex = 0;
};