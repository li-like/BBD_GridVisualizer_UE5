#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SQLiteDatabase.h" // 核心头文件
#include "MyGeoSOTDatabase.generated.h"

UCLASS()
class GEOSOTSYSTEM_API UMyGeoSOTDatabase : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// === 核心功能 ===
	// 1. 初始化数据库和表
	bool InitDatabase();
    
	// 2. 存入障碍物 (用于烘焙)
	void SaveObstacle(int64 GridID, int32 Level, int32 AltLayer);
    
	// 3. 批量事务提交 (提高写入速度)
	void BeginTransaction();
	void CommitTransaction();

	// 4. 查询是否可通过 (用于 A* 算法)
	// 这里我们用内存缓存优化，避免 A* 频繁读盘
	bool IsGridWalkable(int64 GridID);

	// 5. 将数据库加载到内存缓存 (游戏开始时调用)
	void LoadMapToCache();

private:
	FString DbPath;
	FSQLiteDatabase Database;
    
	// 内存缓存：只存障碍物的 ID，用于 O(1) 极速查询
	TSet<int64> ObstacleCache;
};