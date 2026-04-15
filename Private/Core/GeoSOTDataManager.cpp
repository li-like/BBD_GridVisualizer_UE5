#include "Core/GeoSOTDataManager.h"
#include "Core/GeoSOTMath.h"
#include "Fission/GeoSOTRiskEnvironment.h"
#include "CesiumGeoreference.h"
#include "Engine/World.h"

static uint32 AngleToGridIndex(double Angle, int32 Level) {
    uint32 FullInt = static_cast<uint32>((Angle + 256.0) * 8388608.0);
    return FullInt >> (32 - Level);
}
static double GridIndexToCenterAngle(uint32 Index, int32 Level) {
    int32 Shift = 32 - Level;
    uint32 CenterOffset = (Shift > 0) ? (1 << (Shift - 1)) : 0;
    uint32 FullInt = (Index << Shift) | CenterOffset;
    return (static_cast<double>(FullInt) / 8388608.0) - 256.0;
}

UGeoSOTDataManager::UGeoSOTDataManager() {}

void UGeoSOTDataManager::InitializeManager(UWorld* InWorld, ACesiumGeoreference* InGeoRef, UGeoSOTRiskEnvironment* InRiskEnv,
                                           double InMinLat, double InMaxLat, double InMinLon, double InMaxLon, 
                                           double InMinAlt, double InMaxAlt, int32 InTargetLevel)
{
    WorldPtr = InWorld; GeoRefPtr = InGeoRef; RiskEnvPtr = InRiskEnv;
    TargetLevel = InTargetLevel;
    
    // 【新增】接收渲染器传来的高度参数
    MinAlt = InMinAlt;
    MaxAlt = InMaxAlt;
    
    GlobalChunkMap.Empty();

    // 算出目标层级下，用户指定范围的绝对整数索引边界
    GlobalMinLatIdx = AngleToGridIndex(InMinLat, TargetLevel);
    GlobalMaxLatIdx = AngleToGridIndex(InMaxLat, TargetLevel);
    GlobalMinLonIdx = AngleToGridIndex(InMinLon, TargetLevel);
    GlobalMaxLonIdx = AngleToGridIndex(InMaxLon, TargetLevel);
}

void UGeoSOTDataManager::UpdateObserverPosition(const FVector& CameraPos)
{
    if (!GeoRefPtr || !WorldPtr) return;
    ProcessLoadQueue();

    FVector3d CamLLH = GeoRefPtr->TransformUnrealPositionToLongitudeLatitudeHeight((FVector3d)CameraPos);
    
    // 1. 算出相机在 Level 20 下的绝对索引
    uint32 CamLatIdx = AngleToGridIndex(CamLLH.Y, TargetLevel);
    uint32 CamLonIdx = AngleToGridIndex(CamLLH.X, TargetLevel);

    // 2. 算出相机所在的 Chunk 坐标 (极其简单的整数除法！)
    int32 CamChunkX = CamLonIdx / ChunkGridSize;
    int32 CamChunkY = CamLatIdx / ChunkGridSize;

    // 3. 圈定周围 3x3 个 Chunk 进行加载
    for (int32 i = -1; i <= 1; ++i)
    {
        for (int32 j = -1; j <= 1; ++j)
        {
            int32 TargetChunkX = CamChunkX + i;
            int32 TargetChunkY = CamChunkY + j;

            FString ChunkKey = FString::Printf(TEXT("C_%d_%d"), TargetChunkX, TargetChunkY);

            if (!GlobalChunkMap.Contains(ChunkKey))
            {
                FGridChunk NewChunk;
                NewChunk.ChunkKey = ChunkKey;
                NewChunk.State = EChunkState::Unloaded;
                NewChunk.ChunkX = TargetChunkX;
                NewChunk.ChunkY = TargetChunkY;
                
                GlobalChunkMap.Add(ChunkKey, NewChunk);
                ChunkLoadQueue.Enqueue(ChunkKey);
            }
        }
    }
}

void UGeoSOTDataManager::ProcessLoadQueue()
{
    FString KeyToLoad;
    if (ChunkLoadQueue.Dequeue(KeyToLoad)) {
        if (FGridChunk* ChunkPtr = GlobalChunkMap.Find(KeyToLoad)) {
            ChunkPtr->State = EChunkState::Loading;
            BuildChunkNodes(*ChunkPtr);
            ChunkPtr->State = EChunkState::Ready;
            
            // 【新增】记录就绪的区块，准备进行雷达式的轮询扫描
            ReadyChunkKeys.Add(KeyToLoad); 
        }
    }
}

void UGeoSOTDataManager::BuildChunkNodes(FGridChunk& OutChunk)
{
    if (!WorldPtr || !GeoRefPtr) return;

    double SpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel);
    FVector BoxHalfExtent(SpanMeters * 48.0); 
    FCollisionShape OBBShape = FCollisionShape::MakeBox(BoxHalfExtent);
    FCollisionQueryParams QueryParams;
    QueryParams.bTraceComplex = true; 

    int32 AltLayerMin = FMath::FloorToInt(MinAlt / SpanMeters);
    int32 AltLayerMax = FMath::FloorToInt(MaxAlt / SpanMeters);
    OutChunk.Nodes.Reserve(ChunkGridSize * ChunkGridSize * (AltLayerMax - AltLayerMin + 1));

    // 【极其干净的纯整数遍历】
    // 根据 ChunkX 和 ChunkY，直接乘出它内部包含的起始网格索引
    uint32 StartLonIdx = OutChunk.ChunkX * ChunkGridSize;
    uint32 StartLatIdx = OutChunk.ChunkY * ChunkGridSize;

    for (int32 h = AltLayerMin; h <= AltLayerMax; ++h)
    {
        double CenterAlt = (h * SpanMeters) + (SpanMeters * 0.5);

        for (int32 i = 0; i < ChunkGridSize; ++i)
        {
            for (int32 j = 0; j < ChunkGridSize; ++j)
            {
                uint32 CurrentLonIdx = StartLonIdx + i;
                uint32 CurrentLatIdx = StartLatIdx + j;

                // 【边界裁剪】：如果这个网格超出了你在面板上指定的全局范围，直接抛弃！
                if (CurrentLatIdx < GlobalMinLatIdx || CurrentLatIdx > GlobalMaxLatIdx ||
                    CurrentLonIdx < GlobalMinLonIdx || CurrentLonIdx > GlobalMaxLonIdx)
                {
                    continue;
                }

                double CenterLat = GridIndexToCenterAngle(CurrentLatIdx, TargetLevel);
                double CenterLon = GridIndexToCenterAngle(CurrentLonIdx, TargetLevel);

                FGridNodeData NewNode;
                NewNode.CenterLat = CenterLat;
                NewNode.CenterLon = CenterLon;
                NewNode.CenterAlt = CenterAlt;
                NewNode.GeoSOTCode = UGeoSOTMath::EncodeToGeoSOTString(CenterLat, CenterLon, CenterAlt, TargetLevel, true);

                FVector3d Center3D = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat, CenterAlt));
                FVector WorldCenter = (FVector)Center3D;

                FVector3d UpLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat, CenterAlt + 100.0));
                FVector UpDir = (FVector)(UpLLH - Center3D).GetSafeNormal();
                FVector3d NorthLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(CenterLon, CenterLat + 0.01, CenterAlt));
                FVector TempNorthDir = (FVector)(NorthLLH - Center3D).GetSafeNormal();
                
                FQuat WorldQuat = FRotationMatrix::MakeFromXZ(FVector::CrossProduct(TempNorthDir, UpDir).GetSafeNormal(), UpDir).ToQuat();

                bool bHit = WorldPtr->OverlapAnyTestByChannel(WorldCenter, WorldQuat, ECC_Visibility, OBBShape, QueryParams);
                
                NewNode.bIsObstacle = bHit;
                NewNode.Rho_Risk = bHit ? 1.0 : 0.0;
                NewNode.W_Risk = 0.0; NewNode.Q_Risk = 0.0;
                NewNode.Score = 1.0 - (Weight_Rho * NewNode.Rho_Risk);
                
                OutChunk.Nodes.Add(NewNode);
            }
        }
    }
}

// =========================================================
// 核心 3：滚动动态扫描 (解决 Cesium 异步加载导致物理漏判的神技)
// =========================================================
void UGeoSOTDataManager::RollingDynamicScan(float CurrentTimeSeconds)
{
    if (ReadyChunkKeys.Num() == 0 || !RiskEnvPtr || !WorldPtr || !GeoRefPtr) return;

    // 每一帧像雷达扫描一样，挑出一个已经加载的区块进行彻底的物理与环境重新评估
    CurrentScanChunkIndex = (CurrentScanChunkIndex + 1) % ReadyChunkKeys.Num();
    FString KeyToScan = ReadyChunkKeys[CurrentScanChunkIndex];

    if (FGridChunk* ChunkPtr = GlobalChunkMap.Find(KeyToScan))
    {
        double SpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel);
        FVector BoxHalfExtent(SpanMeters * 48.0, SpanMeters * 48.0, 1.0);
        FCollisionShape SweepShape = FCollisionShape::MakeBox(BoxHalfExtent);
        FCollisionQueryParams QueryParams;
        QueryParams.bTraceComplex = true;

        for (FGridNodeData& Node : ChunkPtr->Nodes)
        {
            // 1. 【重新执行物理扫掠】（因为相机飞近了，Cesium 现在把模型加载出来了！）
            FVector3d Center3D = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt));
            FVector WorldCenter = (FVector)Center3D;

            FVector3d UpLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt + 100.0));
            FVector UpDir = (FVector)(UpLLH - Center3D).GetSafeNormal();
            FVector3d NorthLLH = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat + 0.01, Node.CenterAlt));
            FVector TempNorthDir = (FVector)(NorthLLH - Center3D).GetSafeNormal();
            FQuat WorldQuat = FRotationMatrix::MakeFromXZ(FVector::CrossProduct(TempNorthDir, UpDir).GetSafeNormal(), UpDir).ToQuat();

            FVector TopPos = WorldCenter + UpDir * (SpanMeters * 50.0);
            FVector BottomPos = WorldCenter - UpDir * (SpanMeters * 50.0);

            FHitResult HitResult;
            bool bHit = WorldPtr->SweepSingleByChannel(HitResult, TopPos, BottomPos, WorldQuat, ECC_Visibility, SweepShape, QueryParams);

            Node.bIsObstacle = bHit;
            Node.Rho_Risk = bHit ? 1.0 : 0.0;

            // 2. 刷新动态气象与通信风险
            Node.W_Risk = RiskEnvPtr->GetWeatherRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt, CurrentTimeSeconds);
            Node.Q_Risk = RiskEnvPtr->GetCommunicationRisk(Node.CenterLat, Node.CenterLon, Node.CenterAlt);

            // 3. 重新融合打分
            Node.Score = 1.0 - (Weight_Rho * Node.Rho_Risk + Weight_Weather * Node.W_Risk + Weight_Comm * Node.Q_Risk);
            Node.Score = FMath::Clamp(Node.Score, 0.0, 1.0);
        }
    }
}

void UGeoSOTDataManager::GetReadyNodesInRadius(const FVector& CameraPos, double Radius, TArray<FGridNodeData>& OutNodes) const
{
    OutNodes.Empty();
    double RadiusSq = Radius * Radius;
    for (const auto& Pair : GlobalChunkMap) {
        if (Pair.Value.State == EChunkState::Ready) {
            for (const FGridNodeData& Node : Pair.Value.Nodes) {
                FVector3d World3D = GeoRefPtr->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(Node.CenterLon, Node.CenterLat, Node.CenterAlt));
                if (FVector::DistSquaredXY((FVector)World3D, CameraPos) <= RadiusSq) {
                    OutNodes.Add(Node);
                }
            }
        }
    }
}