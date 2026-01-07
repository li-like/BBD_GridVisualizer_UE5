#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Types/GeoSOTTypes.h"
#include "Core/GeoSOTUtils.h"
#include "GeoSOTTestActor.generated.h"

/**
 * 结构体：用于存储国标文档中的测试用例数据
 */
struct FGeoSOTStandardTestCase
{
	FString Name;      // 点名称 (如：墨尔本CBD)
	double Longitude;  // 经度
	double Latitude;   // 纬度
	double Height;     // 高度
	int32 TestLevel;   // 测试层级 (如 15)
	FString StandardCodeHex; // 国标给出的Hex编码 (暂用于记录对比)

	FGeoSOTStandardTestCase(FString InName, double Lon, double Lat, double H, int32 Lv, FString Hex)
		: Name(InName), Longitude(Lon), Latitude(Lat), Height(H), TestLevel(Lv), StandardCodeHex(Hex) {}
};

/**
 * 测试执行器
 * 功能：加载GB/T 39607-2020标准数据，运行核心算法，验证几何准确性
 */
UCLASS()
class GEOSOTSYSTEM_API AGeoSOTTestActor : public AActor
{
	GENERATED_BODY()
    
public:    
	AGeoSOTTestActor();

protected:
	virtual void BeginPlay() override;

public:
	// 主测试入口
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|Test")
	void RunOfficialStandardTests();

private:
	// 执行单个测试点验证
	void VerifyPoint(const FGeoSOTStandardTestCase& TestCase);
    
	// 辅助：判断点是否在包围盒内
	bool IsPointInBox(double P_Lon, double P_Lat, double P_Heigt, const FGeoSOTCell& Cell);
};