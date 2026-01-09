

// Copyright 2020-2021 CesiumGS, Inc. and Contributors

#pragma once

#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Simulation/SwarmManager.h"
#include "GeoSOTPathTester.generated.h"

class AGeoSOTISMManager;

UCLASS()
class GEOSOTSYSTEM_API AGeoSOTPathTester : public AActor
{
	GENERATED_BODY()
    
public:    
	AGeoSOTPathTester();

	// 这是一个按钮，勾选后下一帧触发测试
	UPROPERTY(EditAnywhere, Category = "Test")
	bool bTriggerSearch = false;

	// 引用场景里的 Manager
	UPROPERTY(EditAnywhere, Category = "Test")
	AGeoSOTISMManager* GeoSOTManager;
	
	void DrawGridDebugInfo(int64 GridCode, FString Label, FColor Color);
	void DebugRoundTrip(FVector WorldPos);
	
	// 获取当前测试的起点 GeoSOT 编码
	UFUNCTION(BlueprintPure, Category = "GeoSOT|Debug")
	int64 GetCurrentStartCode() const { return StartCode; }

	// 获取当前测试的终点 GeoSOT 编码
	UFUNCTION(BlueprintPure, Category = "GeoSOT|Debug")
	int64 GetCurrentEndCode() const { return EndCode; }

	// 获取已经计算好的路径 (直接拿来飞，不用再次计算！)
	UFUNCTION(BlueprintPure, Category = "GeoSOT|Debug")
	const TArray<int64>& GetCalculatedPath() const { return PathResult; }
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT|Linkage")
	ASwarmManager* LinkedSwarmManager;

public:    
	virtual void Tick(float DeltaTime) override;
    
private:
	void RunPathFindingTest();
	int64 StartCode;
	int64 EndCode;
	TArray<int64> PathResult;
};