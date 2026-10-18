#pragma once

#include "CoreMinimal.h"
#include "FGFactoryColoringTypes.h"
#include "MKIRoadMarkingPattern.generated.h"

class UMaterialInterface;

/**
 * A road marking pattern (the build-menu customizer's MKI Patterns > Roads). It is a vanilla pattern descriptor, so the
 * customizer lists it, a recipe unlocks it, and the paint gun applies and saves it like any pattern. It applies only to
 * AMKIRoadSplineRoad (mValidBuildables).
 *
 * A vanilla pattern is a stencil texture laid on a foundation's UV1. A fixed texture cannot know a road's lane count or
 * follow its curve, so the road draws this pattern itself: AMKIRoadSplineRoad::ApplyCustomizationData_Native reads it and
 * sweeps the lines as strips along its own spline, at the offsets below, for its own width (BuildMarkings). Lines lie
 * LiftCm above the deck top, with no collision and no shadow, so they never z-fight with the deck or block a trace.
 *
 * The layout is North American: a solid double yellow center line (two lines CenterGapCm apart), dashed white lines
 * between the lanes of each direction (every 8 m lane), and a solid white trim along each edge, from the deck's edge in
 * to MKIRoadEdgeTrimCm (#316; the maintainer: a trim to the edge, not a line near it, marking the non-drivable margin). An odd lane
 * count from 3 is marked as one lane fewer, each wider (3 lanes as two 12 m lanes, 5 as four 10 m lanes), so it has the
 * even pattern; a one-lane road has its edge lines only.
 */
UCLASS(Blueprintable, BlueprintType)
class MKIROADS_API UMKIRoadMarkingPattern : public UFGFactoryCustomizationDescriptor_Pattern
{
	GENERATED_BODY()

public:
	UMKIRoadMarkingPattern();

	/** Width of every line. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float LineWidthCm = 15.f;

	/** Gap between the two yellow center lines. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float CenterGapCm = 15.f;

	/** Distance from each deck edge to where its edge line's middle was; the edge trim reaches half a line past it (#316). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float EdgeInsetCm = 30.f;

	/** Dashed lane lines: painted length, then gap, measured along the road's centerline from its start. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float DashLengthCm = 200.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float DashGapCm = 200.f;

	/** Height of the lines above the deck top, so they never z-fight with it. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	float LiftCm = 0.5f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	TSoftObjectPtr<UMaterialInterface> YellowMaterial;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "MKI Roads|Marking")
	TSoftObjectPtr<UMaterialInterface> WhiteMaterial;
};

/** One painted line of a road marking pattern: its lateral offset from the centerline, and whether it is dashed or yellow. */
struct FMKIRoadMarkingLine
{
	float LateralCm = 0.f;
	bool bYellow = false;
	bool bDashed = false;
	/** Its width across; 0 is the pattern's LineWidthCm (the edge trims are wider, #316). */
	float WidthCm = 0.f;
};

/** How far each edge trim reaches in from the deck's edge (#316): the old edge line's inner side. */
inline float MKIRoadEdgeTrimCm(const UMKIRoadMarkingPattern& Pattern) { return Pattern.EdgeInsetCm + Pattern.LineWidthCm * 0.5f; }

/** The lines of Pattern on a deck of Lanes 8 m lanes (1-10): double yellow center, dashed white lane lines, white edges. */
MKIROADS_API TArray<FMKIRoadMarkingLine> MKIRoadMarkingLines(const UMKIRoadMarkingPattern& Pattern, int32 Lanes, float LaneWidthCm);

/** The North American Road pattern, put on every new road and intersection (#234: the maintainer, the default when laying roads). */
MKIROADS_API TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern> MKIRoadDefaultPattern();
/** The default pattern's object (the North American Road), or null. */
MKIROADS_API const UMKIRoadMarkingPattern* MKIRoadDefaultPatternObject();
