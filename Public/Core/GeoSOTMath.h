#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GeoSOTMath.generated.h"

UCLASS()
class UGeoSOTMath : public UObject
{
	GENERATED_BODY()

public:
	// 路线 A: 国标 32 位字符串
	static FString EncodeToGeoSOTString(double Lat, double Lon, double Alt, int32 Level, bool bIncludeAlt);
    
	// 路线 B: 底层 3D Morton 码
	static int64 EncodeToMorton3D(double Lat, double Lon, double Alt, int32 Level);
	static void DecodeFromMorton3D(int64 Code, int32 Level, double& OutLat, double& OutLon, double& OutAlt);
    
	static void GetFissionChildren(int64 ParentCode, TArray<int64>& OutChildren);
	static double GetGridSpanMeters(int32 Level);
};