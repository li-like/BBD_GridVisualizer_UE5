#include "Render/GeoSOTGridRenderer.h"
#include "Core/GeoSOTMath.h"
#include "Fission/GeoSOTRiskEnvironment.h"
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
	// 实例化并初始化风险环境
	RiskEnv = NewObject<UGeoSOTRiskEnvironment>(this);
	if (RiskEnv)
	{
		RiskEnv->InitializeMicroEnvironment(MinLat, MaxLat, MinLon, MaxLon);
	}
}

bool AGeoSOTGridRenderer::ShouldTickIfViewportsOnly() const
{
	return true;
}

void AGeoSOTGridRenderer::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	Super::Tick(DeltaTime);

    if (!bIsPoolInitialized || GridTextData.Num() == 0) return;

    // =========================================================
    // 1. 动态仿真：时间切片建筑物检测 (解决 Cesium 加载延迟问题)
    // =========================================================
    if (GetWorld() && GridProcMesh && RiskEnv)
    {
        bool bNeedsMeshUpdate = false;
        double SpanMeters = UGeoSOTMath::GetGridSpanMeters(TargetLevel);
        
        // 构建 OBB 的半尺寸 (Half Extent)。乘以 48 而不是 50，是为了留出 2% 的安全间隙，防止网格互相误碰
        FVector BoxHalfExtent(SpanMeters * 48.0); 
        FCollisionShape OBBShape = FCollisionShape::MakeBox(BoxHalfExtent);
        
        FCollisionQueryParams QueryParams;
        QueryParams.AddIgnoredActor(this); // 忽略自身
        QueryParams.bTraceComplex = true;  // Cesium 建议开启复杂碰撞检测

        // 确定这一帧要检测的批次范围
        int32 EndIndex = FMath::Min(CurrentDetectionIndex + DetectionPerFrame, GridTextData.Num());
    	float CurrentTime = GetWorld()->GetTimeSeconds();

        for (int32 i = CurrentDetectionIndex; i < EndIndex; ++i)
        {
            FGridTextInfo& Grid = GridTextData[i];

            // 如果已经是障碍物了，如果是纯静态环境，可以跳过不测。
            // 但为了体现“呼吸效应”（Cesium模型如果被隐藏，网格也能恢复），我们这里持续检测
            
            // a. 算出当前网格中心的绝对世界坐标
            FVector3d Center3D = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(
                FVector3d(Grid.CenterLon, Grid.CenterLat, Grid.CenterAlt)
            );
            FVector WorldCenter = (FVector)Center3D;

            // b. 算出符合地球曲率的局部姿态 (FQuat)
            FVector3d UpLLH = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(
                FVector3d(Grid.CenterLon, Grid.CenterLat, Grid.CenterAlt + 100.0));
            FVector UpDir = (FVector)(UpLLH - Center3D).GetSafeNormal();

            FVector3d NorthLLH = CesiumGeoref->TransformLongitudeLatitudeHeightPositionToUnreal(
                FVector3d(Grid.CenterLon, Grid.CenterLat + 0.01, Grid.CenterAlt));
            FVector TempNorthDir = (FVector)(NorthLLH - Center3D).GetSafeNormal();

            FVector EastDir = FVector::CrossProduct(TempNorthDir, UpDir).GetSafeNormal();
            FVector NorthDir = FVector::CrossProduct(UpDir, EastDir).GetSafeNormal();
            FQuat WorldQuat = FRotationMatrix::MakeFromXZ(EastDir, UpDir).ToQuat();

        	// ---------------------------------------------------------
        	// 步骤 1：获取物理硬约束 (建筑)
        	// ---------------------------------------------------------
        	bool bHit = GetWorld()->OverlapAnyTestByChannel(WorldCenter, WorldQuat, DetectionChannel, OBBShape, QueryParams);
        	double Rho_Risk = bHit ? 1.0 : 0.0;

        	// ---------------------------------------------------------
        	// 步骤 2：获取环境软约束 (调用环境类)
        	// ---------------------------------------------------------
        	double W_Risk = RiskEnv->GetWeatherRisk(Grid.CenterLat, Grid.CenterLon, Grid.CenterAlt, CurrentTime);
        	double Q_Risk = RiskEnv->GetCommunicationRisk(Grid.CenterLat, Grid.CenterLon, Grid.CenterAlt);

        	// ---------------------------------------------------------
        	// 步骤 3：异构融合打分 (执行论文公式)
        	// ---------------------------------------------------------
        	double Score = 1.0 - (Weight_Rho * Rho_Risk + Weight_Weather * W_Risk + Weight_Comm * Q_Risk);
        	Score = FMath::Clamp(Score, 0.0, 1.0);
            
        	Grid.CurrentScore = Score; 

        	// ---------------------------------------------------------
        	// 步骤 4：阈值判断与颜色渲染更新
        	// ---------------------------------------------------------
        	bool bIsDanger = (Score < Threshold_Theta);

        	if (bIsDanger != Grid.bIsObstacle)
        	{
        		Grid.bIsObstacle = bIsDanger;
                
        		// 危险态(待裂变)为亮黄色高不透明度，安全态为绿色极度透明
        		FColor TargetColor = bIsDanger ? FColor(255, 255, 0, 180) : FColor(0, 255, 0, 15);

        		for (int v = 0; v < 24; ++v)
        		{
        			CachedColors[Grid.VertexStartIndex + v] = TargetColor;
        		}
        		bNeedsMeshUpdate = true;
        	}
        }

        // 推进队列指针，循环扫描
        CurrentDetectionIndex = EndIndex;
        if (CurrentDetectionIndex >= GridTextData.Num())
        {
            CurrentDetectionIndex = 0;
        }

        // e. 极其轻量级的渲染更新
        if (bNeedsMeshUpdate)
        {
            TArray<FVector> EmptyNormals;
            TArray<FProcMeshTangent> EmptyTangents;
            // 这个函数只更新传入的数据，速度极快！
            GridProcMesh->UpdateMeshSection(0, CachedVertices, EmptyNormals, CachedUVs, CachedColors, EmptyTangents);
        }
    }

	// --- B. 文字 LOD 渲染 (解决原点漂移与视角跟随) ---
	FVector ViewLocation;
	bool bHasValidView = false;

	if (GetWorld()->HasBegunPlay())
	{
		// 【核心修复 1】：游戏运行状态下，获取真实的摄像机镜头位置，而不是角色的物理身体位置！
		APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
		if (CameraManager)
		{
			ViewLocation = CameraManager->GetCameraLocation(); 
			bHasValidView = true;
		}
	}
    
	// 【核心修复 2】：黑魔法！如果在编辑器里(没点运行)，直接从渲染底层的上一帧抓取视口相机位置
	if (!bHasValidView && GetWorld()->ViewLocationsRenderedLastFrame.Num() > 0)
	{
		ViewLocation = GetWorld()->ViewLocationsRenderedLastFrame[0];
		bHasValidView = true;
	}

	// 终极保底回退
	if (!bHasValidView)
	{
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
	// 【新增】提前在编辑器构造阶段就实例化风险环境
	if (!RiskEnv)
	{
		RiskEnv = NewObject<UGeoSOTRiskEnvironment>(this);
	}
	// 确保参数随时更新
	RiskEnv->InitializeMicroEnvironment(MinLat, MaxLat, MinLon, MaxLon);
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
	
	CachedVertices.Empty();
	CachedUVs.Empty();
	CachedColors.Empty();
    TArray<FVector> AllVertices;
	TArray<FVector2D> AllUVs;
    TArray<int32> AllTriangles;
    
    int32 VertexIndexOffset = 0;
	FColor InitialSafeColor = FColor(0, 255, 0, 15);

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
          	int32 CurrentGridVertexStart = CachedVertices.Num();

          	// 记录当前网格在数组中的起始索引

          	for (int face = 0; face < 6; ++face)
          	{
          		CachedVertices.Add(P[FaceIdx[face][0]]); 
          		CachedVertices.Add(P[FaceIdx[face][1]]); 
          		CachedVertices.Add(P[FaceIdx[face][2]]); 
          		CachedVertices.Add(P[FaceIdx[face][3]]); 

          		CachedUVs.Add(FVector2D(0, 0));
          		CachedUVs.Add(FVector2D(1, 0));
          		CachedUVs.Add(FVector2D(0, 1));
          		CachedUVs.Add(FVector2D(1, 1));

          		// 统一涂上初始绿色
          		CachedColors.Add(InitialSafeColor);
          		CachedColors.Add(InitialSafeColor);
          		CachedColors.Add(InitialSafeColor);
          		CachedColors.Add(InitialSafeColor);

          		AllTriangles.Add(VertexIndexOffset + 0);
          		AllTriangles.Add(VertexIndexOffset + 2);
          		AllTriangles.Add(VertexIndexOffset + 1);

          		AllTriangles.Add(VertexIndexOffset + 1);
          		AllTriangles.Add(VertexIndexOffset + 2);
          		AllTriangles.Add(VertexIndexOffset + 3);

          		VertexIndexOffset += 4;
          	}

          	double RealCenterLat = GridIndexToCenterAngle(i, TargetLevel);
          	double RealCenterLon = GridIndexToCenterAngle(j, TargetLevel);
          	double RealCenterAlt = MinAltVal + (SpanMeters * 0.5);

          	FGridTextInfo TextInfo;
          	TextInfo.CenterLat = RealCenterLat;
          	TextInfo.CenterLon = RealCenterLon;
          	TextInfo.CenterAlt = RealCenterAlt;
          	TextInfo.PrecomputedCodeStr = UGeoSOTMath::EncodeToGeoSOTString(RealCenterLat, RealCenterLon, RealCenterAlt, TargetLevel, true);
             
          	TextInfo.VertexStartIndex = CurrentGridVertexStart;
          	TextInfo.bIsObstacle = false; 
             
          	GridTextData.Add(TextInfo);
          }
       }
    }

	// [修改] 使用缓存数组创建 Mesh Section
	TArray<FVector> EmptyNormals;
	TArray<FProcMeshTangent> EmptyTangents;
	GridProcMesh->CreateMeshSection(0, CachedVertices, AllTriangles, EmptyNormals, CachedUVs, CachedColors, EmptyTangents, false);

	// 重置检测队列指针
	CurrentDetectionIndex = 0;
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
			FString FinalDisplayText = FString::Printf(TEXT("%s\nS: %.2f"), *TargetGrid->PrecomputedCodeStr, TargetGrid->CurrentScore);
          
			// 直接拿预计算好的字符串，不用再每帧 Decode/Encode 了
			TextComp->SetText(FText::FromString(FinalDisplayText));
			TextComp->SetWorldLocation(TextWorldLoc);
			// =========================================================
		  // 【核心修复】实现完美屏幕对齐 (Screen-Aligned Billboarding)
		  // =========================================================
		  APlayerCameraManager* CameraManager = UGameplayStatics::GetPlayerCameraManager(GetWorld(), 0);
		  if (CameraManager)
		  {
			 // 1. 直接获取摄像机当前的世界旋转姿态
			 FRotator CameraRotation = CameraManager->GetCameraRotation();
			 
			 // 2. 修正 UTextRenderComponent 的翻转问题。
			 // 因为 TextRender 默认正面朝向 -X，我们需要将 Yaw 轴旋转 180 度，
			 // 这样文字的局部 -X 轴就会完美对齐摄像机的 X 轴 (即摄像机视线)。
			 FRotator BillboardRot = CameraRotation;
			 BillboardRot.Yaw += 180.f; // 加上 180 度偏航修正
			 
			 // 3. 还可以修正文字倾斜问题 (可选)：
			 // 如果你发现摄像机倾斜时文字也跟着倾斜，可以把 Roll 轴锁死为 0
			 // BillboardRot.Roll = 0.f; 

			 // 4. 将修正后的旋转应用到文字组件上
			 TextComp->SetWorldRotation(BillboardRot);
		  }
		  // =========================================================
			TextComp->SetWorldSize(TextWorldSize); 
			TextComp->SetTextRenderColor(TargetGrid->bIsObstacle ? FColor::Yellow : FColor::Green);
			TextComp->SetVisibility(true);
		}
		else
		{
			TextComp->SetVisibility(false);
		}
	}
	
	
}