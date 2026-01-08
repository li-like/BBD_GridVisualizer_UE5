#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Types/GeoSOTTypes.h"
#include "GeoSOTUtils.generated.h"

class ACesiumGeoreference;

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
	
	// [新增] 将 Row/Col 转换为 GeoSOT Z-order 编码 (64位)
	static int64 GenerateGeoSOTCode(int32 Row, int32 Col, int32 Level, int32 AltIndex);

	// [新增] 解码：从 Code 还原出坐标
	static void DecodeGeoSOTCode(int64 Code, int32& OutRow, int32& OutCol, int32& OutLevel, int32& OutAltLayer);

	// [新增] 获取 3D 邻居 (上下东南西北)
	static void GetNeighbors3D(int64 CurrentCode, int32 Level, TArray<int64>& OutNeighbors);
	
	/**
	 * [新增] 逆向计算：从 Row/Col 算出 GeoSOT 网格中心点的经纬度
	 * 基于 GeoSOT 标准 512度 扩展域 [-256, 256]
	 */
	static void GetGeoSOTCenter(int32 Row, int32 Col, int32 Level, double& OutLat, double& OutLon);

	/**
	 * [新增] 正向计算：从 经纬度 算出对应的 Row/Col 索引
	 * 基于 GeoSOT 标准 512度 扩展域
	 */
	static void LatLonToGeoSOT(double Lat, double Lon, int32 Level, int32& OutRow, int32& OutCol);
	
	// [新增] 核心解耦函数：计算网格在 UE 世界空间中的物理变换
	// 输入：地理引用、网格数据
	// 输出：世界坐标中心、世界旋转四元数、物理缩放
	static bool CalculateGridWorldTransform(
		ACesiumGeoreference* GeoRef,
		const FGeoSOTCell& Cell,
		FVector& OutCenterWorld,
		FQuat& OutRotationWorld,
		FVector& OutScale
	);


private:
	// 计算经纬度索引
	static int64 CalculateLatLonIndex(double Degree, int32 Level);
    
	// 计算高度索引 (基于赤道度数比例)
	static int64 CalculateAltIndex(double HeightMeters, int32 Level);
	
	// [内部工具] 扩张二进制位 (例如 111 -> 10101)
	static uint64 SpreadBits(uint32 x);
	// [内部工具] 收缩二进制位
	static uint32 CompactBits(uint64 x);
};