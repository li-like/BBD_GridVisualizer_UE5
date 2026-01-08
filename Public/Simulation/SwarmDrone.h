#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmDrone.generated.h"

class ACesiumGeoreference;

UCLASS()
class GEOSOTSYSTEM_API ASwarmDrone : public AActor
{
	GENERATED_BODY()
    
public:    
	ASwarmDrone();

protected:
	virtual void BeginPlay() override;

public:    
	virtual void Tick(float DeltaTime) override;

	// --- 核心接口 ---

	/**
	 * 接收一串 GeoSOT 编码，开始飞行
	 * @param PathCodes: 由 PathFinder 算出来的网格 ID 列表
	 * @param InGeoReference: 必须传入 CesiumGeoReference 才能进行坐标逆算
	 */
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|Swarm")
	void ExecuteGeoSOTPath(const TArray<int64>& PathCodes, ACesiumGeoreference* InGeoReference);

private:
	// --- 内部逻辑 ---

	// 将 GeoSOT 编码转换为 UE 世界坐标 (AirSim 需要的 Waypoints)
	void ConvertPathToWaypoints(const TArray<int64>& PathCodes);

	// 移动逻辑 (模拟 AirSim 的 moveOnPath)
	void ProcessFlightControl(float DeltaTime);

private:
	// 飞行状态
	bool bIsFlying = false;
	int32 CurrentWaypointIndex = 0;
    
	// 转换后的路径点 (UE World Coordinates)
	TArray<FVector> Waypoints;

	// 飞行参数
	UPROPERTY(EditAnywhere, Category = "Flight")
	float FlightSpeed = 1000.0f; // 10m/s (UE单位是cm)

	UPROPERTY(EditAnywhere, Category = "Flight")
	float AcceptanceRadius = 100.0f; // 到达半径 1m

	// 缓存的地理参考
	UPROPERTY()
	ACesiumGeoreference* GeoReference;
};