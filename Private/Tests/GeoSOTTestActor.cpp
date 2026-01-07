#include "Tests/GeoSOTTestActor.h"
#include "Core/GeoSOTUtils.h" // 确保引用了核心库

AGeoSOTTestActor::AGeoSOTTestActor()
{
    PrimaryActorTick.bCanEverTick = false;
}

void AGeoSOTTestActor::BeginPlay()
{
    Super::BeginPlay();
    UE_LOG(LogTemp, Display, TEXT("=== GeoSOT 3D Standard Test Suite Started ==="));
    RunOfficialStandardTests();
}

void AGeoSOTTestActor::RunOfficialStandardTests()
{
    // --- 1. 准备国标 GB/T 39607-2020 附录C 测试数据 ---
    TArray<FGeoSOTStandardTestCase> TestCases;

    // 中国区域
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Beijing"), 116.4000, 39.9000, 0.0, 15, TEXT("B032000C00000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Shanghai"), 121.4737, 31.2304, 10.0, 15, TEXT("B03C000000000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Guangzhou"), 113.2644, 23.1291, 15.3, 15, TEXT("B032800000000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Urumqi"), 87.6168, 43.8256, 800.0, 15, TEXT("B024000000000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Sansha"), 112.3331, 16.8409, 2.0, 15, TEXT("B01E000000000000")));

    // 全球区域
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("Melbourne CBD"), 144.9631, -37.8136, 50.0, 15, TEXT("B32000C000000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("London"), -0.1278, 51.5074, 25.0, 15, TEXT("B038000000000000")));
    TestCases.Add(FGeoSOTStandardTestCase(TEXT("New York"), -74.0060, 40.7128, 10.0, 15, TEXT("B026000000000000")));
    
    // --- 2. 循环执行验证 ---
    for (const auto& TestCase : TestCases)
    {
        VerifyPoint(TestCase);
    }
}

void AGeoSOTTestActor::VerifyPoint(const FGeoSOTStandardTestCase& TestCase)
{
    // 步骤 A: 模拟输入范围 (微小范围以命中单点)
    double Epsilon = 1e-7;
    FVector2D StartPos(TestCase.Longitude - Epsilon, TestCase.Latitude - Epsilon);
    FVector2D EndPos(TestCase.Longitude + Epsilon, TestCase.Latitude + Epsilon);
    
    // 高度范围也给一个微小量
    double MinH = TestCase.Height - 0.1;
    double MaxH = TestCase.Height + 0.1;

    // 步骤 B: 调用新的 3D 核心算法
    // [修复 Error 1] GenerateGridsByRange -> GenerateGridsByRange3D
    TArray<FGeoSOTCell> Results = UGeoSOTUtils::GenerateGridsByRange3D(
        StartPos, 
        EndPos, 
        MinH, 
        MaxH, 
        TestCase.TestLevel
    );

    // 步骤 C: 验证结果
    if (Results.Num() == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("[FAIL] %s: No 3D Grid Generated!"), *TestCase.Name);
        return;
    }

    bool bFoundValidGrid = false;
    FGeoSOTCell ValidCell;

    for (const auto& Cell : Results)
    {
        if (IsPointInBox(TestCase.Longitude, TestCase.Latitude, TestCase.Height, Cell))
        {
            ValidCell = Cell;
            bFoundValidGrid = true;
            break;
        }
    }

    if (bFoundValidGrid)
    {
        // [修复 Error 2] GetGridSpan -> GetDegSpan / GetHeightSpan
        double DegSpan = UGeoSOTUtils::GetDegSpan(TestCase.TestLevel);
        double HeightSpan = UGeoSOTUtils::GetHeightSpan(TestCase.TestLevel);
        
        // 格式化输出日志 (增加 Alt 信息)
        FString LogMsg = FString::Printf(
            TEXT("[PASS] %s | Lat/Lon/Alt: (%.4f, %.4f, %.2fm) | Level: %d\n"
                 "       -> 3D Grid Code: Row=%lld, Col=%lld, Alt=%lld\n"
                 "       -> Bounds: Lat[%.5f~%.5f], Lon[%.5f~%.5f], H[%.1f~%.1f]"), 
            *TestCase.Name, 
            TestCase.Latitude, TestCase.Longitude, TestCase.Height, TestCase.TestLevel,
            ValidCell.Code.Row, ValidCell.Code.Col, ValidCell.Code.Alt,
            ValidCell.MinLat, ValidCell.MaxLat, ValidCell.MinLon, ValidCell.MaxLon,
            ValidCell.MinHeight, ValidCell.MaxHeight
        );
        
        UE_LOG(LogTemp, Log, TEXT("%s"), *LogMsg);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[FAIL] %s: Point not inside generated grids!"), *TestCase.Name);
    }
}

// [新增] 支持高度检查
bool AGeoSOTTestActor::IsPointInBox(double P_Lon, double P_Lat, double P_Alt, const FGeoSOTCell& Cell)
{
    double Tolerance = 1e-9;
    
    bool bLatOK = (P_Lat >= Cell.MinLat - Tolerance) && (P_Lat <= Cell.MaxLat + Tolerance);
    bool bLonOK = (P_Lon >= Cell.MinLon - Tolerance) && (P_Lon <= Cell.MaxLon + Tolerance);
    
    // 验证高度是否在层级对应的物理高度区间内
    bool bAltOK = (P_Alt >= Cell.MinHeight - Tolerance) && (P_Alt <= Cell.MaxHeight + Tolerance);
    
    return bLatOK && bLonOK && bAltOK;
}