#include "Render/GeoSOTGridRenderer.h"
#include "Core/GeoSOTMath.h"
#include "Kismet/GameplayStatics.h"

// =========================================================
// 内部纯整数转换辅助函数 (绝对无浮点缝隙)
// =========================================================
static uint32 AngleToGridIndex(double Angle, int32 Level)
{
    uint32 FullInt = static_cast<uint32>((Angle + 256.0) * 8388608.0);
    return FullInt >> (32 - Level);
}

static double GridIndexToCenterAngle(uint32 Index, int32 Level)
{
    int32 Shift = 32 - Level;
    uint32 CenterOffset = (Shift > 0) ? (1 << (Shift - 1)) : 0;
    uint32 FullInt = (Index << Shift) | CenterOffset;
    return (static_cast<double>(FullInt) / 8388608.0) - 256.0;
}

// [新增] 获取网格边界经纬度 (用于生成 8 个物理顶点)
static double GridIndexToBoundaryAngle(uint32 Index, int32 Level)
{
	int32 Shift = 32 - Level;
	uint32 BoundaryInt = Index << Shift;
	return (static_cast<double>(BoundaryInt) / 8388608.0) - 256.0;
}

// ---------------------------------------------------------

AGeoSOTGridRenderer::AGeoSOTGridRenderer()
{
	PrimaryActorTick.bCanEverTick = true;

	// 初始化程序化网格
	GridProcMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("GridProcMesh"));
	RootComponent = GridProcMesh;
	GridProcMesh->bUseAsyncCooking = true;
    
	// 你可以在这里加载你的线框材质
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> AcademicMat(TEXT("你的材质路径"));
	if (AcademicMat.Succeeded())
	{
		GridProcMesh->SetMaterial(0, AcademicMat.Object);
	}
}

void AGeoSOTGridRenderer::BeginPlay()
{
    Super::BeginPlay();
    InitializeTextPool();
}

bool AGeoSOTGridRenderer::ShouldTickIfViewportsOnly() const
{
	return true;
}

void AGeoSOTGridRenderer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bIsPoolInitialized || GridTextData.Num() == 0) return;

	FVector ViewLocation;
	bool bHasValidView = false;

	// 判断是否在游戏运行中
	if (GetWorld()->HasBegunPlay())
	{
		APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(GetWorld(), 0);
		if (PlayerPawn)
		{
			ViewLocation = PlayerPawn->GetActorLocation(); // 或者是 PlayerCameraManager 的位置
			bHasValidView = true;
		}
	}
	else
	{
		// [核心修复] 如果在编辑器里没点运行，我们使用网格生成的锚点(Actor位置)作为 LOD 的中心
		// 这样只要你把相机拉到网格附近，中心区域的文字就会强制显示出来预览
		ViewLocation = GetActorLocation(); 
		bHasValidView = true;
	}

	if (bHasValidView)
	{
		UpdateTextLOD(ViewLocation);
	}
}
#if WITH_EDITOR
void AGeoSOTGridRenderer::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
    
	// 当在面板中修改任何属性时，重新生成网格
	GenerateAndRenderGrids();
}
#endif

void AGeoSOTGridRenderer::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// 确保在编辑器拖动或编译时，对象池和网格都是最新的
	InitializeTextPool();
	GenerateAndRenderGrids();
}
void AGeoSOTGridRenderer::ClearGrids()
{
	if (GridProcMesh) GridProcMesh->ClearAllMeshSections();
	GridTextData.Empty();
	for (UTextRenderComponent* TextComp : TextPool)
	{
		if (TextComp) TextComp->SetVisibility(false);
	}
}

// =========================================================
// 核心：一键纯整数剖分生成 (基于论文等长体素模型)
// =========================================================
void AGeoSOTGridRenderer::GenerateAndRenderGrids()
{
    if (!CesiumGeoref) return;
    ClearGrids();

    double SpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel); 
    
    uint32 LatIdxMin = AngleToGridIndex(MinLat, TargetLevel);
    uint32 LatIdxMax = AngleToGridIndex(MaxLat, TargetLevel);
    uint32 LonIdxMin = AngleToGridIndex(MinLon, TargetLevel);
    uint32 LonIdxMax = AngleToGridIndex(MaxLon, TargetLevel);

    int32 AltLayerMin = FMath::FloorToInt(MinAlt / SpanMeters);
    int32 AltLayerMax = FMath::FloorToInt(MaxAlt / SpanMeters);

    // 锚定 Actor 自身位置 (用原点)
    double OriginLat = GridIndexToCenterAngle(LatIdxMin, TargetLevel);
    double OriginLon = GridIndexToCenterAngle(LonIdxMin, TargetLevel);
    double OriginAlt = (AltLayerMin * SpanMeters) + (SpanMeters * 0.5);
    FVector3d OriginWorld = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(FVector3d(OriginLon, OriginLat, OriginAlt));
    SetActorLocation((FVector)OriginWorld);
    
    // 我们将所有的顶点和面片数据收集到这两个大数组中，一次性渲染
    TArray<FVector> AllVertices;
	TArray<FVector2D> AllUVs;
    TArray<int32> AllTriangles;
    
    int32 VertexIndexOffset = 0;

    for (int32 h = AltLayerMin; h <= AltLayerMax; ++h)
    {
       for (uint32 i = LatIdxMin; i <= LatIdxMax; ++i)
       {
          for (uint32 j = LonIdxMin; j <= LonIdxMax; ++j)
          {
             // 1. 获取网格的真实经纬高范围 (用于计算 8 个顶点)
             double MinLonDeg = GridIndexToBoundaryAngle(j, TargetLevel);
             double MaxLonDeg = GridIndexToBoundaryAngle(j + 1, TargetLevel);
             double MinLatDeg = GridIndexToBoundaryAngle(i, TargetLevel);
             double MaxLatDeg = GridIndexToBoundaryAngle(i + 1, TargetLevel);
             
             double MinAltVal = h * SpanMeters;
             double MaxAltVal = (h + 1) * SpanMeters;

             // 2. 将 8 个球面 LLH 坐标，转换为虚幻世界坐标
             FVector3d LLH[8] = {
                 FVector3d(MinLonDeg, MinLatDeg, MinAltVal), // 0: 左后下
                 FVector3d(MaxLonDeg, MinLatDeg, MinAltVal), // 1: 右后下
                 FVector3d(MinLonDeg, MaxLatDeg, MinAltVal), // 2: 左前下
                 FVector3d(MaxLonDeg, MaxLatDeg, MinAltVal), // 3: 右前下
                 FVector3d(MinLonDeg, MinLatDeg, MaxAltVal), // 4: 左后上
                 FVector3d(MaxLonDeg, MinLatDeg, MaxAltVal), // 5: 右后上
                 FVector3d(MinLonDeg, MaxLatDeg, MaxAltVal), // 6: 左前上
                 FVector3d(MaxLonDeg, MaxLatDeg, MaxAltVal)  // 7: 右前上
             };

             // 3. 将世界坐标转化为相对于 Actor 的局部坐标，压入顶点数组
          	// 3. 将世界坐标转化为相对于 Actor 的局部坐标
          	FTransform ActorInvTransform = GetActorTransform().Inverse();
          	FVector P[8];
          	for (int k = 0; k < 8; ++k)
          	{
          		FVector WorldPt = (FVector)CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(LLH[k]);
          		P[k] = ActorInvTransform.TransformPosition(WorldPt);
          	}

          	// 4. 定义 6 个面的顶点组合 (为了赋予独立 UV，必须生成 24 个顶点)
          	int32 FaceIdx[6][4] = {
          		{0, 1, 2, 3}, // Bottom
				  {4, 5, 6, 7}, // Top
				  {0, 1, 4, 5}, // Front/Back 1
				  {2, 3, 6, 7}, // Front/Back 2
				  {0, 2, 4, 6}, // Left
				  {1, 3, 5, 7}  // Right
          	};

          	// 逐面生成顶点、UV 和三角形
          	for (int face = 0; face < 6; ++face)
          	{
          		// 添加该面的 4 个顶点
          		AllVertices.Add(P[FaceIdx[face][0]]); 
          		AllVertices.Add(P[FaceIdx[face][1]]); 
          		AllVertices.Add(P[FaceIdx[face][2]]); 
          		AllVertices.Add(P[FaceIdx[face][3]]); 

          		// 补全缺失的 UV (左下, 右下, 左上, 右上)
          		AllUVs.Add(FVector2D(0, 0));
          		AllUVs.Add(FVector2D(1, 0));
          		AllUVs.Add(FVector2D(0, 1));
          		AllUVs.Add(FVector2D(1, 1));

          		// 构建该面的 2 个三角形 (注意偏移量)
          		AllTriangles.Add(VertexIndexOffset + 0);
          		AllTriangles.Add(VertexIndexOffset + 2);
          		AllTriangles.Add(VertexIndexOffset + 1);

          		AllTriangles.Add(VertexIndexOffset + 1);
          		AllTriangles.Add(VertexIndexOffset + 2);
          		AllTriangles.Add(VertexIndexOffset + 3);

          		VertexIndexOffset += 4;
          	}

             // 5. 生成逻辑层数据 (文字中心点永远在最中间)
             double RealCenterLat = GridIndexToCenterAngle(i, TargetLevel);
             double RealCenterLon = GridIndexToCenterAngle(j, TargetLevel);
             double RealCenterAlt = MinAltVal + (SpanMeters * 0.5);

             FGridTextInfo TextInfo;
             TextInfo.CenterLat = RealCenterLat;
             TextInfo.CenterLon = RealCenterLon;
             TextInfo.CenterAlt = RealCenterAlt;
             TextInfo.PrecomputedCodeStr = UGeoSOTMath::EncodeToGeoSOTString(RealCenterLat, RealCenterLon, RealCenterAlt, TargetLevel, true);
             
             GridTextData.Add(TextInfo);
          }
       }
    }

    // 将计算好的所有点和面，一次性交给虚幻引擎生成网格
    TArray<FVector> EmptyNormals;
    TArray<FVector2D> EmptyUVs;
    TArray<FColor> EmptyColors;
    TArray<FProcMeshTangent> EmptyTangents;

	GridProcMesh->CreateMeshSection(0, AllVertices, AllTriangles, EmptyNormals, AllUVs, EmptyColors, EmptyTangents, false);
}

// =========================================================
// LOD 对象池与文本动态渲染
// =========================================================

void AGeoSOTGridRenderer::InitializeTextPool()
{
    if (bIsPoolInitialized) return;

    for (int32 i = 0; i < MaxTextPoolSize; ++i)
    {
       FName CompName = MakeUniqueObjectName(this, UTextRenderComponent::StaticClass(), TEXT("PoolText"));
       UTextRenderComponent* TextComp = NewObject<UTextRenderComponent>(this, CompName);
       
       TextComp->RegisterComponent();
       TextComp->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
       
       TextComp->SetText(FText::GetEmpty());
       TextComp->SetVisibility(false);
       
       // [修改] 文字对齐方式改为绝对居中
       TextComp->SetHorizontalAlignment(EHTA_Center);
       TextComp->SetVerticalAlignment(EVRTA_TextCenter); 
       
       TextComp->SetTextRenderColor(FColor::Green);
       
       TextPool.Add(TextComp);
    }
    bIsPoolInitialized = true;
}

// 辅助排序结构体
struct FDistanceItem
{
    double DistSq;
    const FGridTextInfo* Info;
    bool operator<(const FDistanceItem& Other) const { return DistSq < Other.DistSq; }
};

void AGeoSOTGridRenderer::UpdateTextLOD(const FVector& PlayerLocation)
{
	TArray<FDistanceItem> Candidates;
	double DistSqLimit = TextVisibleDistance * TextVisibleDistance;

	for (const FGridTextInfo& Grid : GridTextData)
	{
		// 【核心修复】每一帧都根据 Cesium 当前的地理参考，实时计算出正确的虚幻世界坐标！
		FVector3d RealTimeWorld3D = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(
			FVector3d(Grid.CenterLon, Grid.CenterLat, Grid.CenterAlt)
		);
		FVector RealTimeWorldPos = (FVector)RealTimeWorld3D;

		double DistSq = FVector::DistSquared(RealTimeWorldPos, PlayerLocation);
		if (DistSq < DistSqLimit)
		{
			Candidates.Add({DistSq, &Grid}); // 传递引用时注意，我们只存指针用于后续提取数据
		}
	}

	Candidates.Sort();
	int32 NumToDisplay = FMath::Min(Candidates.Num(), TextPool.Num());

	for (int32 i = 0; i < TextPool.Num(); ++i)
	{
		UTextRenderComponent* TextComp = TextPool[i];

		if (i < NumToDisplay)
		{
			const FGridTextInfo* TargetGrid = Candidates[i].Info;

			// 重新算一次实时位置，赋给文字（因为之前只参与了距离判断）
			FVector3d TextWorldLoc3D = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(
				FVector3d(TargetGrid->CenterLon, TargetGrid->CenterLat, TargetGrid->CenterAlt)
			);
			FVector TextWorldLoc = (FVector)TextWorldLoc3D;

			FVector DirToPlayer = PlayerLocation - TextWorldLoc;
			if (DirToPlayer.IsNearlyZero()) DirToPlayer = FVector::UpVector;
          
			// 直接拿预计算好的字符串，不用再每帧 Decode/Encode 了
			TextComp->SetText(FText::FromString(TargetGrid->PrecomputedCodeStr));
			TextComp->SetWorldLocation(TextWorldLoc);
			TextComp->SetWorldRotation((-DirToPlayer).Rotation()); 
			TextComp->SetWorldSize(TextWorldSize); 
			TextComp->SetVisibility(true);
		}
		else
		{
			TextComp->SetVisibility(false);
		}
	}
}