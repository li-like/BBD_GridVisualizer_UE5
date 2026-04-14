// Copyright 2020-2021 CesiumGS, Inc. and Contributors

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GeoSOTGridRenderer.generated.h"

UCLASS()
class GEOSOTSYSTEM_API AGeoSOTGridRenderer : public AActor
{
	GENERATED_BODY()

public:
	// Sets default values for this actor's properties
	AGeoSOTGridRenderer();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:
	// Called every frame
	virtual void Tick(float DeltaTime) override;
};
