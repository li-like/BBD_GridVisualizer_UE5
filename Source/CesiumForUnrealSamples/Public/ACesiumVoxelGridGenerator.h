#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

// --- 包含 CesiumGeoreference ---
// 这是您参考代码中使用的核心类
#include "CesiumGeoreference.h" 

#include "ACesiumVoxelGridGenerator.generated.h"

UCLASS()
class CESIUMFORUNREALSAMPLES_API ACesiumVoxelGridGenerator : public AActor
{
    GENERATED_BODY()

public:
    ACesiumVoxelGridGenerator();

protected:
    virtual void BeginPlay() override;

public:
    virtual void Tick(float DeltaTime) override;

    // --- 【关键组件】 ---
    // 参照 PlaneTrack，我们需要持有 CesiumGeoreference 的引用
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cesium")
    ACesiumGeoreference* CesiumGeoreference;

    // --- 【设置参数】 ---
    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MinLongitude = 116.390;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MaxLongitude = 116.400;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MinLatitude = 39.910;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MaxLatitude = 39.920;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MinAltitude = 100.0;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double MaxAltitude = 300.0;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double StepLongitude = 0.002;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double StepLatitude = 0.002;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    double StepAltitude = 50.0;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    FLinearColor GridColor = FLinearColor::Red;

    UPROPERTY(EditAnywhere, Category = "Voxel Grid Settings")
    float LineThickness = 5.0f;

private:
    void DrawVoxelGrid();

    // 辅助函数：封装具体的转换调用
    FVector GetUnrealLocation(double Longitude, double Latitude, double Height) const;

    bool IsVoxelOccupied(const FVector& Center, const FVector& HalfExtents) const;
};