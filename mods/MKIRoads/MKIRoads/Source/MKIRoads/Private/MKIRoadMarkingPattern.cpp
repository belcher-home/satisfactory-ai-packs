#include "MKIRoadMarkingPattern.h"

#include "MKIRoadSplineRoad.h"
#include "ProceduralMeshComponent.h"

#include "MKIRoadJunction.h"
#include "MKIRoadTile.h"
#include "MKIRoadSplineRoad.h"
#include "Materials/MaterialInterface.h"

UMKIRoadMarkingPattern::UMKIRoadMarkingPattern()
{
	// Only the spline road and the junctions (#227) draw these lines; vanilla foundations would only see an unknown stencil ID.
	// A piece's squares too: paint aimed at one goes to its whole piece (the maintainer, 2026-10-18).
	mValidBuildables = { AMKIRoadSplineRoad::StaticClass(), AMKIRoadJunction::StaticClass(), AMKIRoadTile::StaticClass() };
	YellowMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/MKIRoads/Materials/M_MKIR_RoadPaint_Yellow.M_MKIR_RoadPaint_Yellow")));
	WhiteMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/MKIRoads/Materials/M_MKIR_RoadPaint_White.M_MKIR_RoadPaint_White")));
}

TArray<FMKIRoadMarkingLine> MKIRoadMarkingLines(const UMKIRoadMarkingPattern& Pattern, int32 Lanes, float LaneWidthCm)
{
	TArray<FMKIRoadMarkingLine> Lines;
	const float Half = Lanes * LaneWidthCm * 0.5f, PairOffset = (Pattern.CenterGapCm + Pattern.LineWidthCm) * 0.5f;
	// Roads take 2 or 4 lanes (#256), the same number each way.
	for (int32 Boundary = 1; Boundary < Lanes; ++Boundary)
	{
		const float Lateral = -Half + Boundary * LaneWidthCm;
		// The double solid yellow center line, the pair CenterGapCm apart; dashed white between the lanes of each direction.
		if (Boundary * 2 == Lanes) for (const float Side : { -1.f, 1.f }) Lines.Add({ Side * PairOffset, true, false });
		else Lines.Add({ Lateral, false, true });
	}
	// The solid white edge trims, from each edge in to MKIRoadEdgeTrimCm (#316).
	const float Trim = MKIRoadEdgeTrimCm(Pattern);
	for (const float Side : { -1.f, 1.f }) Lines.Add({ Side * (Half - Trim * 0.5f), false, false, Trim });
	return Lines;
}

const UMKIRoadMarkingPattern* MKIRoadDefaultPatternObject()
{
	const TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> Pattern = MKIRoadDefaultPattern();
	return Pattern ? Cast<UMKIRoadMarkingPattern>(Pattern->GetDefaultObject()) : nullptr;
}

void MKIRoadApplyPaint(UProceduralMeshComponent* Mesh, const UMKIRoadMarkingPattern& Pattern, const FMKIRoadDeckGeometry (&Paint)[2])
{
	if (!Mesh) return;
	Mesh->ClearAllMeshSections();
	const TSoftObjectPtr<UMaterialInterface> Materials[2] = { Pattern.YellowMaterial, Pattern.WhiteMaterial };
	for (int32 Section = 0; Section < 2; ++Section)
	{
		TArray<FProcMeshTangent> Tangents;
		for (const FVector& Tangent : Paint[Section].Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
		Mesh->CreateMeshSection_LinearColor(Section, Paint[Section].Vertices, Paint[Section].Triangles, Paint[Section].Normals, Paint[Section].UVs, Paint[Section].UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
		if (UMaterialInterface* Material = Materials[Section].LoadSynchronous()) Mesh->SetMaterial(Section, Material);
	}
}

TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> MKIRoadDefaultPattern()
{
	return LoadClass<UFGFactoryCustomizationDescriptor_Pattern>(nullptr, TEXT("/MKIRoads/Customization/PatternDesc_MKIR_NorthAmericanRoad.PatternDesc_MKIR_NorthAmericanRoad_C"));
}
