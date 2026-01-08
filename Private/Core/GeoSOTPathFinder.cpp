#include "Core/GeoSOTPathFinder.h"

#include "Core/GeoSOTUtils.h"
#include "Core/MyGeoSOTDatabase.h"
#include "Kismet/GameplayStatics.h"

bool UGeoSOTPathFinder::FindPath(const UObject* WorldContext, int64 StartCode, int64 EndCode, TArray<int64>& OutPathCodes)
{
    // 1. 获取数据库 (检查碰撞用)
    UMyGeoSOTDatabase* DB = nullptr;
    if (UGameInstance* GI = UGameplayStatics::GetGameInstance(WorldContext))
    {
        DB = GI->GetSubsystem<UMyGeoSOTDatabase>();
    }
    if (!DB) return false;

    // 起点或终点如果是障碍，直接放弃
    if (!DB->IsGridWalkable(StartCode) || !DB->IsGridWalkable(EndCode)) 
    {
        //UE_LOG(LogTemp, Warning, TEXT("Start or End is blocked!"));
        return false;
    }

    // A* 数据结构
    TArray<FNodeRecord> OpenSet;    // 待检查列表
    TMap<int64, FNodeRecord> AllNodes; // 所有已发现节点的记录 (充当 ClosedSet + 记录表)

    // 初始化起点
    FNodeRecord StartNode;
    StartNode.Code = StartCode;
    StartNode.ParentCode = -1;
    StartNode.G = 0;
    StartNode.H = Heuristic(StartCode, EndCode);
    
    OpenSet.Add(StartNode);
    AllNodes.Add(StartCode, StartNode);

    int32 MaxSteps = 5000; // 安全熔断，防止死循环
    int32 Steps = 0;

    while (OpenSet.Num() > 0)
    {
        if (++Steps > MaxSteps) 
        {
            //UE_LOG(LogTemp, Warning, TEXT("Pathfinding timeout!"));
            break;
        }

        // 2. 取出 F 值最小的节点 (简单的排序模拟优先队列)
        OpenSet.Sort([](const FNodeRecord& A, const FNodeRecord& B) {
            return A.F() < B.F();
        });

        FNodeRecord Current = OpenSet[0];
        OpenSet.RemoveAt(0); // Pop

        // 3. 到达终点？
        if (Current.Code == EndCode)
        {
            // 回溯路径
            int64 Trace = EndCode;
            while (Trace != -1)
            {
                OutPathCodes.Insert(Trace, 0); // 插入头部
                if (AllNodes.Contains(Trace))
                    Trace = AllNodes[Trace].ParentCode;
                else
                    break;
            }
            return true;
        }

        // 4. 扩展邻居
        TArray<int64> Neighbors;
        UGeoSOTUtils::GetNeighbors3D(Current.Code, 21, Neighbors); // 假设 Level 21

        for (int64 NeighborCode : Neighbors)
        {
            // 4.1 碰撞检测 (查数据库缓存)
            // 如果不可走，跳过
            if (!DB->IsGridWalkable(NeighborCode)) continue;

            // 4.2 计算 G 值 (邻居距离为 1)
            float NewG = Current.G + 1.0f;

            // 4.3 如果是新节点，或者找到了更短的路
            if (!AllNodes.Contains(NeighborCode) || NewG < AllNodes[NeighborCode].G)
            {
                FNodeRecord NeighborNode;
                NeighborNode.Code = NeighborCode;
                NeighborNode.ParentCode = Current.Code;
                NeighborNode.G = NewG;
                NeighborNode.H = Heuristic(NeighborCode, EndCode);

                AllNodes.Add(NeighborCode, NeighborNode);
                
                // 加入 OpenSet (这里简单处理，不去重，严格来说应该更新 OpenSet 里已有的)
                OpenSet.Add(NeighborNode);
            }
        }
    }

    return false;
}

float UGeoSOTPathFinder::Heuristic(int64 A, int64 B)
{
    // 解码算出坐标差
    int32 R1, C1, L1, H1;
    int32 R2, C2, L2, H2;
    UGeoSOTUtils::DecodeGeoSOTCode(A, R1, C1, L1, H1);
    UGeoSOTUtils::DecodeGeoSOTCode(B, R2, C2, L2, H2);

    // 曼哈顿距离 (Manhattan Distance)
    return FMath::Abs(R1 - R2) + FMath::Abs(C1 - C2) + FMath::Abs(H1 - H2);
}