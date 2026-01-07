#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "GeoSOTViewTest.generated.h"

// 定义一个简单的结构体来存储单个网格的信息
struct FGeoGridInfo
{
	FVector CenterLocation; // 网格中心的世界坐标
	FVector Extent;         // 网格的半长宽高 (Box Extent)
	bool bIsOccupied;       // 核心逻辑：是否有建筑物阻挡

	FGeoGridInfo() : CenterLocation(FVector::ZeroVector), Extent(FVector(50)), bIsOccupied(false) {}
};

UCLASS()
class GEOSOTSYSTEM_API AGeoSOTViewTest : public AActor
{
	GENERATED_BODY()
    
public:    
	AGeoSOTViewTest();

protected:
	virtual void BeginPlay() override;

public:    
	// --- 核心配置参数 ---
    
	// 网格的边长 (单位: cm, 假如是GeoSOT第n级，这里手动指定对应的物理尺寸)
	UPROPERTY(EditAnywhere, Category = "GeoSOT Config")
	float GridSize;

	// 生成区域的长宽范围 (单位: 个数，例如 10 代表生成 10x10xHeight 的区域)
	UPROPERTY(EditAnywhere, Category = "GeoSOT Config")
	int32 SearchRangeXY;

	// 高度方向的层数
	UPROPERTY(EditAnywhere, Category = "GeoSOT Config")
	int32 SearchRangeZ;
	
	UFUNCTION(CallInEditor, Category = "Test")
	void RunGeoSOTTest();

	// --- 组件 ---
	UPROPERTY(VisibleAnywhere)
	UProceduralMeshComponent* ProcMeshComp;

	// --- 核心数据存储 ---
	// 在单线程中，我们用一个大数组暂存所有网格数据
	TArray<FGeoGridInfo> GridDataArray;

private:
	// --- 核心流程函数 ---
    
	// 步骤1：生成网格数据（不涉及物理，纯数学计算）
	void GenerateGridData();

	// 步骤2：建筑物判断（对GridDataArray进行遍历检测）
	void CheckGridCollision();

	// 步骤3：渲染（根据Data状态绘制不同颜色的Mesh）
	void RenderGrids();

	// 辅助：绘制一个立方体的顶点索引
	void AddCubeToMesh(const FVector& Center, const FVector& Extent, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector2D>& UVs, TArray<FColor>& Colors, FColor Color);
};