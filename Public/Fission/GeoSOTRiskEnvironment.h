#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "GeoSOTRiskEnvironment.generated.h"

USTRUCT()
struct FVirtualBaseStation
{
	GENERATED_BODY()
	double Lat;
	double Lon;
	double Alt;
	double MaxRangeMeters;
};

UCLASS()
class UGeoSOTRiskEnvironment : public UObject
{
	GENERATED_BODY()

public:
	UGeoSOTRiskEnvironment();

	void InitializeMicroEnvironment(double MinLat, double MaxLat, double MinLon, double MaxLon);
    
	// 获取动态气象与静态通信风险
	double GetWeatherRisk(double Lat, double Lon, double Alt, float TimeSeconds) const;
	double GetCommunicationRisk(double Lat, double Lon, double Alt) const;

private:
	double CalculateDistanceMeters(double Lat1, double Lon1, double Lat2, double Lon2, double Alt1, double Alt2) const;

	TArray<FVirtualBaseStation> BaseStations;
};