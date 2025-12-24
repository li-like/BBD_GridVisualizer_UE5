// Copyright 2020-2021 CesiumGS, Inc. and Contributors


#include "PlaneTrack.h"

// Sets default values
APlaneTrack::APlaneTrack()
{
	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;

	// Initialize the track
	SplineTrack = CreateDefaultSubobject<USplineComponent>(TEXT("SplineTrack"));
	// This lets us visualize the spline in Play mode
	SplineTrack->SetDrawDebug(true);

	// Set the color of the spline
	SplineTrack->SetUnselectedSplineSegmentColor(FLinearColor(1.f, 0.f, 0.f));
}

// Called when the game starts or when spawned
void APlaneTrack::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void APlaneTrack::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void APlaneTrack::LoadSplineTrackPoints()
{
    if (this->AircraftsRawDataTable != nullptr && this->CesiumGeoreference != nullptr)
    {
        int32 PointIndex = 0;
        for (auto& row : this->AircraftsRawDataTable->GetRowMap())
        {
            FAircraftRawData* Point = (FAircraftRawData*)row.Value;
            // Get row data point in lat/long/alt and transform it into UE points
            double PointLatitude = Point->Latitude;
            double PointLongitude = Point->Longitude;
            double PointHeight = Point->Height;

            // Compute the position in UE coordinates
            FVector SplinePointPosition = this->CesiumGeoreference->TransformLongitudeLatitudeHeightPositionToUnreal(FVector(PointLongitude, PointLatitude, PointHeight));
            this->SplineTrack->AddSplinePointAtIndex(SplinePointPosition, PointIndex, ESplineCoordinateSpace::World, false);

            // Get the up vector at the position to orient the aircraft
            const CesiumGeospatial::Ellipsoid& Ellipsoid = CesiumGeospatial::Ellipsoid::WGS84;
            glm::dvec3 upVector = Ellipsoid.geodeticSurfaceNormal(CesiumGeospatial::Cartographic(FMath::DegreesToRadians(PointLongitude),
                FMath::DegreesToRadians(PointLatitude),
                FMath::DegreesToRadians(PointHeight)));

            // Compute the up vector at each point to correctly orient the plane 
            FVector4 ecefUp(upVector.x, upVector.y, upVector.z, 0.0);
            FMatrix ecefToUnreal = this->CesiumGeoreference->ComputeEarthCenteredEarthFixedToUnrealTransformation();			  FVector4 unrealUp = ecefToUnreal.TransformFVector4(ecefUp);
            this->SplineTrack->SetUpVectorAtSplinePoint(PointIndex, FVector(unrealUp), ESplineCoordinateSpace::World, false);

            PointIndex++;
        }
        this->SplineTrack->UpdateSpline();
    }
}

