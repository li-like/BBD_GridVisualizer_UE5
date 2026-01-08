// Copyright 2020-2021 CesiumGS, Inc. and Contributors

#pragma once

#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
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

public:    
	virtual void Tick(float DeltaTime) override;
    
private:
	void RunPathFindingTest();
};