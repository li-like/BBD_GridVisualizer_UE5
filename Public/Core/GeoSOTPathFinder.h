#pragma once
#include "CoreMinimal.h"
#include "MyGeoSOTDatabase.h" // 引用数据库系统
#include "GeoSOTPathFinder.generated.h"

// 节点记录结构体
struct FNodeRecord
{
	int64 Code;
	int64 ParentCode; // 父节点 (用于回溯路径)
	float G; // 离起点的代价
	float H; // 离终点的预估代价 (启发)
	float F() const { return G + H; }
};

UCLASS(BlueprintType)
class GEOSOTSYSTEM_API UGeoSOTPathFinder : public UObject
{
	GENERATED_BODY()

public:
	// 核心寻路函数
	// 返回 true 表示找到路径
	UFUNCTION(BlueprintCallable, Category = "GeoSOT|PathFinding", meta = (AutoCreateRefTerm = "OccupiedGrids"))
	static bool FindPath(
		const UObject* WorldContext, 
		int64 StartCode, 
		int64 EndCode, 
		TArray<int64>& OutPathCodes,
		const TSet<int64>& OccupiedGrids // <--- 移除这里的 = TSet<int64>()
	);

private:
	// 曼哈顿距离启发式
	static float Heuristic(int64 A, int64 B);
};
