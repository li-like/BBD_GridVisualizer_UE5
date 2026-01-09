#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// 引入网络库头文件
#include "Networking.h" 
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "SwarmDrone.generated.h"

class ACesiumGeoreference;

UCLASS()
class GEOSOTSYSTEM_API ASwarmDrone : public AActor
{
    GENERATED_BODY()
    
public:    
    ASwarmDrone();
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // --- 核心入口 ---
    // Manager 调用此函数传入路径
    UFUNCTION(BlueprintCallable, Category = "GeoSOT|Swarm")
    void ExecuteGeoSOTPath(const TArray<int64>& PathCodes, ACesiumGeoreference* InGeoReference);

private:
    // 将 GeoSOT 编码转为 UE 世界坐标
    void ConvertPathToWaypoints(const TArray<int64>& PathCodes);
    
    // 通过 Socket 发送数据给 Python
    void SendPathToPython(); 

private:
    // 缓存计算出的路径点 (UE 坐标系)
    TArray<FVector> Waypoints;
    
    // 地理参考引用
    ACesiumGeoreference* GeoReference;
};