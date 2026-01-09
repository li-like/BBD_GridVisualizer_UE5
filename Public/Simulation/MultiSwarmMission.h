#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MultiSwarmMission.generated.h"

// 前向声明，减少头文件依赖
class AGeoSOTISMManager;
class ACesiumGeoreference;

UCLASS()
class GEOSOTSYSTEM_API AMultiSwarmMission : public AActor
{
	GENERATED_BODY()
    
public:    
	AMultiSwarmMission();

	// --- 核心入口 ---
	// [关键修改] 添加 CallInEditor，这样在编辑器里选中这个 Actor 时，
	// Details 面板会出现一个 "Run Mission" 的按钮，点击即可触发，无需 Play
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GeoSOT|Mission")
	void RunMission();

protected:
	virtual void BeginPlay() override;

private:
	// 1. 场景搜索函数
	AActor* FindActorByTag(FName Tag);
    
	// 2. 单机寻路辅助函数
	bool CalculatePathForDrone(
		FString DroneName, 
		FVector StartPos, 
		FVector EndPos, 
		TArray<FVector>& OutPathPoints,
		TSet<int64>& GlobalOccupiedSet
	);

	// 3. 发送 JSON
	void SendMissionToPython(FString JsonPayload);

private:
	// 定义无人机编队偏移量
	TMap<FString, FVector> DroneOffsets;

	// 引用场景组件
	UPROPERTY()
	AGeoSOTISMManager* ISMManager;
    
	UPROPERTY()
	ACesiumGeoreference* GeoRef;
};