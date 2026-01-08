#include "Core/MyGeoSOTDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFilemanager.h"
// [新增] 必须包含这个头文件才能使用 FSQLitePreparedStatement
#include "SQLitePreparedStatement.h"

void UMyGeoSOTDatabase::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    // 数据库路径: Saved/GeoSOT.db
    DbPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("GeoSOT.db"));
    
    if (InitDatabase())
    {
        //UE_LOG(LogTemp, Log, TEXT("[DB] Database initialized at: %s"), *DbPath);
        LoadMapToCache(); // 启动时加载缓存
    }
}

void UMyGeoSOTDatabase::Deinitialize()
{
    Database.Close();
    Super::Deinitialize();
}

bool UMyGeoSOTDatabase::InitDatabase()
{
    if (!Database.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
    {
        //UE_LOG(LogTemp, Error, TEXT("[DB] Failed to open database!"));
        return false;
    }

    // 创建表结构 (按照我们之前的单表设计)
    // 忽略 ObjType 等非核心字段，先只搞定 A* 需要的数据
    FString CreateTableSQL = TEXT(R"(
        CREATE TABLE IF NOT EXISTS GeoSOT_Global_Map (
            GridID INTEGER PRIMARY KEY, 
            GeoLevel INTEGER, 
            AltLayer INTEGER, 
            IsObstacle INTEGER DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_search ON GeoSOT_Global_Map (GeoLevel, AltLayer);
    )");

    return Database.Execute(*CreateTableSQL);
}

void UMyGeoSOTDatabase::BeginTransaction()
{
    Database.Execute(TEXT("BEGIN TRANSACTION;"));
}

void UMyGeoSOTDatabase::CommitTransaction()
{
    Database.Execute(TEXT("COMMIT;"));
}

void UMyGeoSOTDatabase::SaveObstacle(int64 GridID, int32 Level, int32 AltLayer)
{
    // 如果已经在缓存里，就不写数据库了，节省 IO
    if (ObstacleCache.Contains(GridID)) return;

    FString InsertSQL = FString::Printf(
        TEXT("INSERT OR IGNORE INTO GeoSOT_Global_Map (GridID, GeoLevel, AltLayer, IsObstacle) VALUES (%lld, %d, %d, 1);"),
        GridID, Level, AltLayer);

    Database.Execute(*InsertSQL);
    ObstacleCache.Add(GridID); // 同步更新缓存
}

void UMyGeoSOTDatabase::LoadMapToCache()
{
    ObstacleCache.Empty();
    
    FString QuerySQL = TEXT("SELECT GridID FROM GeoSOT_Global_Map WHERE IsObstacle = 1;");
    
    // 1. 创建语句
    FSQLitePreparedStatement Statement = Database.PrepareStatement(*QuerySQL, ESQLitePreparedStatementFlags::None);
    
    if (Statement.IsValid())
    {
        // 2. 遍历每一行
        while (Statement.Step() == ESQLitePreparedStatementStepResult::Row)
        {
            // === [修复核心] 使用“引用传出”的方式获取数据 ===
            int64 OutID = 0;
            
            // 这里的 TEXT("GridID") 是列名，OutID 是接收数据的变量
            if (Statement.GetColumnValueByName(TEXT("GridID"), OutID))
            {
                ObstacleCache.Add(OutID);
            }
        }
        Statement.Destroy();
        
        // Statement 析构时会自动释放资源
    }
    else
    {
        //UE_LOG(LogTemp, Error, TEXT("[DB] Failed to prepare statement: %s"), *Database.GetLastError());
    }

    //UE_LOG(LogTemp, Log, TEXT("[DB] Loaded %d obstacles into cache."), ObstacleCache.Num());
}

bool UMyGeoSOTDatabase::IsGridWalkable(int64 GridID)
{
    // 如果在障碍物缓存里，就是不可走
    return !ObstacleCache.Contains(GridID);
}