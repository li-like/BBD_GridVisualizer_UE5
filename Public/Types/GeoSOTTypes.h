#pragma once

#include "CoreMinimal.h"
#include "GeoSOTTypes.generated.h"

/**
 * GeoSOT 三维网格编码结构体
 * 包含：层级 (Level), 纬度 (Row), 经度 (Col), 高度 (Alt)
 */
USTRUCT(BlueprintType)
struct FGeoSOTCode
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT")
    int32 Level;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT")
    int64 Row; // Lat Index

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT")
    int64 Col; // Lon Index

    // [新增] 高度维度的整数索引
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GeoSOT")
    int64 Alt; // Alt Index

    FGeoSOTCode() : Level(0), Row(0), Col(0), Alt(0) {}
    FGeoSOTCode(int32 L, int64 R, int64 C, int64 A) : Level(L), Row(R), Col(C), Alt(A) {}

    FString ToString() const
    {
        return FString::Printf(TEXT("L:%d [R:%lld, C:%lld, A:%lld]"), Level, Row, Col, Alt);
    }
    
    // 重载 == 操作符，方便对比
    bool operator==(const FGeoSOTCode& Other) const
    {
        return Level == Other.Level && Row == Other.Row && Col == Other.Col && Alt == Other.Alt;
    }
};

/**
 * GeoSOT 3D 体素单元
 */
USTRUCT(BlueprintType)
struct FGeoSOTCell
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FGeoSOTCode Code;

    // 经纬度边界
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MinLon;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MaxLon;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MinLat;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MaxLat;

    // 高度边界 (由 GeoSOT 层级决定的物理高度，而非用户输入)
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MinHeight;
    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    double MaxHeight;

    // 获取中心点 (3D)
    FVector GetCenter() const 
    {
        return FVector((MinLon + MaxLon) * 0.5, (MinLat + MaxLat) * 0.5, (MinHeight + MaxHeight) * 0.5);
    }
};