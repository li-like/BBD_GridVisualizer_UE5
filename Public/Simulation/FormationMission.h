#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "FormationMission.generated.h"

// 前向声明
class AGeoSOTISMManager;
class ACesiumGeoreference;

UCLASS()
class GEOSOTSYSTEM_API AFormationMission : public AActor
{
	GENERATED_BODY()
    
public:    
	AFormationMission();
	virtual void Tick(float DeltaTime) override;

	// 核心入口：在编辑器点击运行
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "GeoSOT|Mission")
	void RunFormationMission();

private:
	// 辅助函数：根据 Tag 查找 Actor
	AActor* FindActorByTag(FName Tag);

	// [新增] 专门为中心路径算路的辅助函数
	// 注意：这里需要 TSet<int64> 作为参数
	bool CalculatePathForCenter(
		FVector StartPos, 
		FVector EndPos, 
		TArray<FVector>& OutPathPoints,
		TArray<int64>& OutPathCodes,
		TSet<int64>& OccupiedSet
	);

	// 发送 JSON 到 Python
	void SendMissionToPython(FString JsonPayload);

private:
	// 偏移量配置
	TMap<FString, FVector> DroneOffsets;

	// 引用
	UPROPERTY()
	AGeoSOTISMManager* ISMManager;
    
	UPROPERTY()
	ACesiumGeoreference* GeoRef;

	// 轨迹绘制相关
	TMap<FString, FVector> LastPositions;
	bool bMissionRunning = false;
};