#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Types/GeoSOTTypes.h"
#include "GeoSOTUtils.generated.h"

UCLASS()
class GEOSOTSYSTEM_API UGeoSOTUtils : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// GeoSOT 定义：赤道处 1度 ≈ 111319.49 米
	static constexpr double METERS_PER_DEGREE = 111319.49079327358;
	static constexpr double GEOSOT_MAX_RANGE = 512.0;
	static constexpr double GEOSOT_OFFSET = 256.0;

	/** * [3D核心] 生成指定经纬度范围 + 高度范围内的 3D 体素网格 
	 * 严格按照 GeoSOT 3D 剖分逻辑
	 */
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|Core")
	static TArray<FGeoSOTCell> GenerateGridsByRange3D(
		FVector2D StartLonLat, 
		FVector2D EndLonLat, 
		double MinHeight, 
		double MaxHeight, 
		int32 Level
	);

	/** * [3D核心] 获取视野半径内的网格 (包含 Z 轴裁剪)
	 * 用于渲染剔除
	 */
	static TArray<FGeoSOTCell> GetGridsInRadius3D(
		double CenterLon, 
		double CenterLat, 
		double CenterAlt,
		double RadiusMeters, 
		int32 Level
	);

	/** 获取指定层级的经度/纬度跨度 (度) */
	UFUNCTION(BlueprintPure, Category = "GeoSOT|Core")
	static double GetDegSpan(int32 Level);

	/** 获取指定层级的高度跨度 (米) */
	UFUNCTION(BlueprintPure, Category = "GeoSOT|Core")
	static double GetHeightSpan(int32 Level);

private:
	// 计算经纬度索引
	static int64 CalculateLatLonIndex(double Degree, int32 Level);
    
	// 计算高度索引 (基于赤道度数比例)
	static int64 CalculateAltIndex(double HeightMeters, int32 Level);
};