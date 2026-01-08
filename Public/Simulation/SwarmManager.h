#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SwarmManager.generated.h"

class AGeoSOTPathTester;
// 前向声明
class ASwarmDrone;
class ACesiumGeoreference;

UCLASS()
class GEOSOTSYSTEM_API ASwarmManager : public AActor
{
	GENERATED_BODY()
    
public:    
	ASwarmManager();

protected:
	virtual void BeginPlay() override;

public:    
	virtual void Tick(float DeltaTime) override;
	
	// [新增] 联动开关：是否直接使用 PathTester 的路径？
	UPROPERTY(EditAnywhere, Category = "GeoSOT|Swarm")
	bool bSyncWithTester = true;
	
	// [新增] 指定场景中的测试器
	UPROPERTY(EditAnywhere, Category = "GeoSOT|Swarm", meta=(EditCondition="bSyncWithTester"))
	AGeoSOTPathTester* TargetTester;
	
	// 确保这个函数是 public 的
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|Swarm")
	void SpawnAndCommandDrone(const TArray<int64>& PathCodes);

	/** * [测试接口] 运行一次完整的集群测试流程
	 * 1. 自动寻找场景中的 GeoReference
	 * 2. 生成起终点编码
	 * 3. 调用 PathFinder 寻路
	 * 4. 生成无人机并下达飞行指令
	 */
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|Swarm")
	void RunSwarmTest();



private:
	// 场景中的地理参考锚点
	UPROPERTY(VisibleAnywhere, Category = "GeoSOT")
	ACesiumGeoreference* GeoReference;

	// 当前管理的无人机列表 (目前先只测试一架)
	UPROPERTY()
	ASwarmDrone* TestDrone;

	// 辅助：获取 GeoReference
	void FindGeoReference();
};