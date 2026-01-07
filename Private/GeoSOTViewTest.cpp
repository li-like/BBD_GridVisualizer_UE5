#include "GeoSOTViewTest.h"
#include "Kismet/KismetSystemLibrary.h"
#include "DrawDebugHelpers.h" // 记得在文件顶部添加头文件

AGeoSOTViewTest::AGeoSOTViewTest()
{
    PrimaryActorTick.bCanEverTick = false; // 不需要每帧Tick，只在开始时运行一次

    ProcMeshComp = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("ProcMeshComp"));
    RootComponent = ProcMeshComp;

    // 默认参数初始化
    GridSize = 500.0f; // 5米一个格子
    SearchRangeXY = 10; // 10x10范围
    SearchRangeZ = 3;   // 3层高度
}

void AGeoSOTViewTest::BeginPlay()
{
    Super::BeginPlay();

    // // 记录开始时间，用于简单的性能监控
    // double StartTime = FPlatformTime::Seconds();
    //
    // // --- 线性工作流 ---
    //
    // // 1. 生成数据
    // GenerateGridData();
    //
    // // 2. 环境检测
    // CheckGridCollision();
    //
    // // 3. 渲染可视化
    // RenderGrids();
    //
    // double EndTime = FPlatformTime::Seconds();
    // UE_LOG(LogTemp, Warning, TEXT("GeoSOT Generation Finished. Total Grids: %d, Time Cost: %f s"), GridDataArray.Num(), EndTime - StartTime);
}
// 2. 实现这个新函数
void AGeoSOTViewTest::RunGeoSOTTest()
{
    // 强制清除旧数据
    GridDataArray.Empty();
    ProcMeshComp->ClearAllMeshSections();

    // 重新执行全套流程
    GenerateGridData();
    CheckGridCollision();
    RenderGrids();
    
    // 强制确保材质挂载（防止掉材质）
    // 记得改成你材质的真实路径
    // UMaterial* Mat = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, TEXT("/Game/Materials/M_GeoSOT_VertexColor")));
    // if (Mat) ProcMeshComp->SetMaterial(0, Mat);

    UE_LOG(LogTemp, Warning, TEXT("Manual Test Triggered!"));
}
// 步骤 1: 纯生成坐标数据
void AGeoSOTViewTest::GenerateGridData()
{
    GridDataArray.Empty();

    FVector ActorLocation = GetActorLocation();
    FVector HalfSize = FVector(GridSize * 0.5f);

    // 简单的三维循环，模拟区域填充
    // 这里的逻辑可以替换为你真实的 GeoSOT 编码反算逻辑
    for (int32 x = -SearchRangeXY; x <= SearchRangeXY; x++)
    {
        for (int32 y = -SearchRangeXY; y <= SearchRangeXY; y++)
        {
            for (int32 z = 0; z < SearchRangeZ; z++)
            {
                FGeoGridInfo NewGrid;
                
                // 计算每个格子的中心世界坐标
                // 如果是GeoSOT，这里应该是：Code -> Lat/Lon/Alt -> WorldPos
                FVector Offset(x * GridSize, y * GridSize, z * GridSize);
                NewGrid.CenterLocation = ActorLocation + Offset;
                NewGrid.Extent = HalfSize * 0.95f; //稍微缩小一点点，渲染时有缝隙，方便看清楚边界
                NewGrid.bIsOccupied = false; // 默认为空

                GridDataArray.Add(NewGrid);
            }
        }
    }
}

void AGeoSOTViewTest::CheckGridCollision()
{
    TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes;
    ObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_WorldStatic));
    ObjectTypes.Add(UEngineTypes::ConvertToObjectType(ECC_WorldDynamic));
    
    TArray<AActor*> ActorsToIgnore;
    ActorsToIgnore.Add(this);

    for (FGeoGridInfo& Grid : GridDataArray)
    {
        TArray<AActor*> OverlappingActors;
        bool bHasOverlap = UKismetSystemLibrary::BoxOverlapActors(
            this,
            Grid.CenterLocation,
            Grid.Extent,
            ObjectTypes,
            nullptr,
            ActorsToIgnore,
            OverlappingActors
        );

        Grid.bIsOccupied = (OverlappingActors.Num() > 0);

        // --- 添加调试线段 ---
        // 如果检测到碰撞，在中心画一个蓝色的点，方便我们在后台确认逻辑
        if (Grid.bIsOccupied)
        {
            DrawDebugPoint(GetWorld(), Grid.CenterLocation, 20.0f, FColor::Blue, false, 10.0f);
        }
    }
}

// 步骤 3: 渲染
void AGeoSOTViewTest::RenderGrids()
{
    if (GridDataArray.Num() == 0) return;

    // 准备 Mesh 数据
    TArray<FVector> Vertices;
    TArray<int32> Triangles;
    TArray<FVector2D> UVs;
    TArray<FColor> Colors;
    TArray<FVector> Normals;
    TArray<FProcMeshTangent> Tangents;

    // 遍历所有数据生成顶点
    for (const FGeoGridInfo& Grid : GridDataArray)
    {
        // 策略：
        // 如果被占用(Occupied) -> 渲染成红色
        // 如果是空的(Free)     -> 渲染成绿色 (或者半透明线框，这里用实体演示)
        
        FColor GridColor = Grid.bIsOccupied ? FColor::Red : FColor::Green;
        
        // 只有当它是障碍物或者你为了调试想看所有格子时才绘制
        // 这里我们全部绘制，但实际项目中通常只绘制“可用路径”或“障碍物”
        AddCubeToMesh(Grid.CenterLocation, Grid.Extent, Vertices, Triangles, UVs, Colors, GridColor);
    }

    // 提交给 Procedural Mesh
    ProcMeshComp->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
    
    
    
    // 设置简单的材质（如果没有材质，这步可以省略，会显示默认灰格子但有顶点色）
    // 你可以在蓝图中给这个组件设置一个支持 VertexColor 的材质
}

// 辅助函数：画立方体
void AGeoSOTViewTest::AddCubeToMesh(const FVector& Center, const FVector& Extent, TArray<FVector>& Vertices, TArray<int32>& Triangles, TArray<FVector2D>& UVs, TArray<FColor>& Colors, FColor Color)
{
    int32 BaseIndex = Vertices.Num();

    FVector X = FVector(Extent.X, 0, 0);
    FVector Y = FVector(0, Extent.Y, 0);
    FVector Z = FVector(0, 0, Extent.Z);

    // 8个顶点
    FVector v0 = Center - X - Y - Z;
    FVector v1 = Center + X - Y - Z;
    FVector v2 = Center + X + Y - Z;
    FVector v3 = Center - X + Y - Z;
    FVector v4 = Center - X - Y + Z;
    FVector v5 = Center + X - Y + Z;
    FVector v6 = Center + X + Y + Z;
    FVector v7 = Center - X + Y + Z;

    // 添加顶点 (简单的Cube不需要复杂的法线平滑，直接加进去)
    // 这里为了偷懒，直接加点，UV暂时不重要
    // 注意：Procedural Mesh为了Flat Shading通常需要把每个面的顶点独立（共24个顶点），
    // 但为了代码极其简短，我这里共用顶点（8个），这会导致光照看起来圆滑怪异，但用于Debug颜色显示足够了。
    
    // 如果想要纯色块，其实不用共用顶点比较好，这里我们写一个最简单的六面体逻辑
    
    // 下面为了代码量少，还是采用共用顶点方式，如果颜色显示有问题，请告诉我，我再展开写24顶点版
    Vertices.Add(v0); Vertices.Add(v1); Vertices.Add(v2); Vertices.Add(v3);
    Vertices.Add(v4); Vertices.Add(v5); Vertices.Add(v6); Vertices.Add(v7);

    for(int i=0; i<8; i++) 
    {
        Colors.Add(Color);
        UVs.Add(FVector2D(0,0)); // 占位
    }

    // 三角形索引 (顺时针)
    // Bottom
    Triangles.Add(BaseIndex + 0); Triangles.Add(BaseIndex + 2); Triangles.Add(BaseIndex + 1);
    Triangles.Add(BaseIndex + 0); Triangles.Add(BaseIndex + 3); Triangles.Add(BaseIndex + 2);
    
    // Top
    Triangles.Add(BaseIndex + 4); Triangles.Add(BaseIndex + 5); Triangles.Add(BaseIndex + 6);
    Triangles.Add(BaseIndex + 4); Triangles.Add(BaseIndex + 6); Triangles.Add(BaseIndex + 7);

    // Front
    Triangles.Add(BaseIndex + 0); Triangles.Add(BaseIndex + 1); Triangles.Add(BaseIndex + 5);
    Triangles.Add(BaseIndex + 0); Triangles.Add(BaseIndex + 5); Triangles.Add(BaseIndex + 4);

    // Back
    Triangles.Add(BaseIndex + 2); Triangles.Add(BaseIndex + 3); Triangles.Add(BaseIndex + 7);
    Triangles.Add(BaseIndex + 2); Triangles.Add(BaseIndex + 7); Triangles.Add(BaseIndex + 6);

    // Left
    Triangles.Add(BaseIndex + 3); Triangles.Add(BaseIndex + 0); Triangles.Add(BaseIndex + 4);
    Triangles.Add(BaseIndex + 3); Triangles.Add(BaseIndex + 4); Triangles.Add(BaseIndex + 7);

    // Right
    Triangles.Add(BaseIndex + 1); Triangles.Add(BaseIndex + 2); Triangles.Add(BaseIndex + 6);
    Triangles.Add(BaseIndex + 1); Triangles.Add(BaseIndex + 6); Triangles.Add(BaseIndex + 5);
}