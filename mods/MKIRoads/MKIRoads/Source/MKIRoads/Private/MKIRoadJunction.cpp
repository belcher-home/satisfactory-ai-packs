#include "MKIRoadJunction.h"
#include "MKIRoadTile.h"

#include "MKIRoadAutoPaths.h"
#include "MKIRoadsGameWorldModule.h"
#include "MKIRoadJunctionHologram.h"
#include "WheeledVehicles/FGVehiclePathSegment.h"
#include "MKIRoadMarkingPattern.h"
#include "MKIRoadSupports.h"
#include "MKIRoadLegendGlyphs.generated.h"
#include "FGCustomizationRecipe.h"
#include "FGDismantleInterface.h"
#include "FGFactoryLegsComponent.h"
#include "Buildables/FGBuildableDockingStation.h"
#include "EngineUtils.h"
#include "WheeledVehicles/FGVehiclePathNode.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "ProceduralMeshComponent.h"
#include "TimerManager.h"

namespace
{
	/** Edge key: the two endpoints rounded to whole centimetres, in a fixed order. */
	TTuple<int32, int32, int32, int32> EdgeKey(const FVector2D& A, const FVector2D& B)
	{
		const FIntPoint P(FMath::RoundToInt32(A.X), FMath::RoundToInt32(A.Y)), Q(FMath::RoundToInt32(B.X), FMath::RoundToInt32(B.Y));
		const bool bSwap = P.X > Q.X || (P.X == Q.X && P.Y > Q.Y);
		return bSwap ? MakeTuple(Q.X, Q.Y, P.X, P.Y) : MakeTuple(P.X, P.Y, Q.X, Q.Y);
	}

	/**
	 * The outward normal of a cell's edge P to Q (in the cell's vertex order), from the cell's winding. The maintainer (#232): with
	 * the vertex average as the inside, a rounded corner's average lay past its curb (most of its vertices are on the arc), so
	 * its side faces pointed in (culled, the sides missing) and its edge lines stood off the deck.
	 */
	FVector2D OutwardNormal(const TArray<FVector2D>& Cell, const FVector2D& P, const FVector2D& Q)
	{
		double Twice = 0.0;
		for (int32 Index = 0; Index < Cell.Num(); ++Index) Twice += FVector2D::CrossProduct(Cell[Index], Cell[(Index + 1) % Cell.Num()]);
		// Counter-clockwise (positive area): the inside is on the left of P to Q, so the outside on the right.
		const FVector2D Right = FVector2D(Q.Y - P.Y, P.X - Q.X).GetSafeNormal();
		return Twice >= 0.0 ? Right : -Right;
	}

	/** A turnaround's lane (#236): its arriving lane's lateral offset (right of the arm's outward direction negative), its
	 * loop's radius, and the x along the arm where it starts to veer onto the loop. */
	struct FTurnLane
	{
		float Lateral = 0.f;
		float Loop = 0.f;
		float VeerStart = 0.f;
	};

	TArray<FTurnLane> TurnLanes(int32 Lanes)
	{
		const int32 Count = AMKIRoadJunction::IsSupportedLaneCount(Lanes) ? Lanes : AMKIRoadSplineRoad::DefaultLanes;
		const float Width = AMKIRoadSplineRoad::LaneWidthCm, Veer = AMKIRoadJunction::TurnVeerRadiusCm;
		TArray<FTurnLane> Out;
		for (int32 Lane = 0; Lane < Count / 2; ++Lane)
		{
			FTurnLane Item;
			Item.Lateral = (Lane + 0.5f) * Width;
			Item.Loop = AMKIRoadJunction::TurnLoopRadiusCm + Lane * Width;
			// The veer arc's center is Veer right of the lane at VeerStart, and touches the loop from outside: its distance to
			// the loop's center is Loop + Veer.
			Item.VeerStart = FMath::Sqrt(FMath::Square(Item.Loop + Veer) - FMath::Square(Item.Lateral + Veer));
			Out.Add(Item);
		}
		return Out;
	}

	/** The part of a convex polygon on one side of the line Dot(P, Normal) = Offset (below it, or above). */
	TArray<FVector2D> ClipHalfPlane(const TArray<FVector2D>& In, const FVector2D& Normal, double Offset, bool bKeepBelow)
	{
		TArray<FVector2D> Out;
		auto Side = [&Normal, Offset](const FVector2D& Point) { return FVector2D::DotProduct(Point, Normal) - Offset; };
		auto Keeps = [&Side, bKeepBelow](const FVector2D& Point) { return bKeepBelow ? Side(Point) <= 0.0 : Side(Point) >= 0.0; };
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			const FVector2D A = In[Index], B = In[(Index + 1) % In.Num()];
			if (Keeps(A)) Out.Add(A);
			if (Keeps(A) != Keeps(B)) Out.Add(A + (B - A) * (Side(A) / (Side(A) - Side(B))));
		}
		return Out;
	}

	/** The convex hull of Points, counter-clockwise (Andrew's monotone chain). */
	TArray<FVector2D> ConvexHull(TArray<FVector2D> Points)
	{
		Points.Sort([](const FVector2D& A, const FVector2D& B) { return A.X < B.X || (A.X == B.X && A.Y < B.Y); });
		TArray<FVector2D> Hull;
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const int32 Start = Hull.Num();
			for (int32 Step = 0; Step < Points.Num(); ++Step)
			{
				const FVector2D& Point = Points[Pass == 0 ? Step : Points.Num() - 1 - Step];
				while (Hull.Num() >= Start + 2 && FVector2D::CrossProduct(Hull.Last() - Hull[Hull.Num() - 2], Point - Hull[Hull.Num() - 2]) <= 0.0) Hull.Pop();
				Hull.Add(Point);
			}
			Hull.Pop();
		}
		return Hull;
	}

	/** Where the ray from the origin through Point leaves Box (the origin inside it). */
	FVector2D RayToBox(const FBox2D& Box, const FVector2D& Point)
	{
		const FVector2D Direction = Point.GetSafeNormal();
		double Reach = TNumericLimits<double>::Max();
		if (Direction.X > 1e-9) Reach = FMath::Min(Reach, Box.Max.X / Direction.X); else if (Direction.X < -1e-9) Reach = FMath::Min(Reach, Box.Min.X / Direction.X);
		if (Direction.Y > 1e-9) Reach = FMath::Min(Reach, Box.Max.Y / Direction.Y); else if (Direction.Y < -1e-9) Reach = FMath::Min(Reach, Box.Min.Y / Direction.Y);
		return Direction * Reach;
	}

	/** Area of a polygon, either winding. */
	double CellArea(const TArray<FVector2D>& Cell)
	{
		double Twice = 0.0;
		for (int32 Index = 0; Index < Cell.Num(); ++Index) Twice += FVector2D::CrossProduct(Cell[Index], Cell[(Index + 1) % Cell.Num()]);
		return FMath::Abs(Twice) * 0.5;
	}

	/** A whole 8 m tile: four corners spanning one tile each way. */
	bool IsWholeTile(const TArray<FVector2D>& Cell)
	{
		if (Cell.Num() != 4) return false;
		const FBox2D Box(Cell);
		const FVector2D Size = Box.GetSize();
		return FMath::Abs(Size.X - AMKIRoadSplineRoad::LaneWidthCm) < 1.0 && FMath::Abs(Size.Y - AMKIRoadSplineRoad::LaneWidthCm) < 1.0 && CellArea(Cell) > Size.X * Size.Y - 1.0;
	}

	/**
	 * A turnaround's deck (#236): whole 8 m tiles from the arm end back, the least grid square that holds its round end
	 * (#314; the maintainer: the round hull left gaps no foundation could fill). The tiles run on from the arm's own, so the deck
	 * meets foundations and roads laid on the same grid.
	 */
	void TurnaroundCells(FMKIRoadJunctionLayout& Out, int32 Lanes)
	{
		const float Tile = AMKIRoadSplineRoad::LaneWidthCm, Half = Lanes * Tile * 0.5f;
		const float ArmEnd = AMKIRoadJunction::TurnaroundArmEndCm(Lanes), Radius = AMKIRoadJunction::TurnaroundDeckRadiusCm(Lanes);
		const int32 Back = FMath::CeilToInt32((ArmEnd + Radius) / Tile - 1e-3);
		const int32 Side = FMath::Max(0, FMath::CeilToInt32((Radius - Half) / Tile - 1e-3));
		for (int32 Column = 0; Column < Back; ++Column)
		{
			const double X0 = ArmEnd - (Column + 1) * Tile;
			for (int32 Row = -Side; Row < Lanes + Side; ++Row)
			{
				const double Y0 = -Half + Row * Tile;
				Out.Cells.Add({ FVector2D(X0, Y0), FVector2D(X0 + Tile, Y0), FVector2D(X0 + Tile, Y0 + Tile), FVector2D(X0, Y0 + Tile) });
				Out.TileOrigins.Add(FVector2D(X0, Y0));
			}
		}
		Out.Arms.Add({ FVector2D(ArmEnd, 0.0), FVector2D(1.0, 0.0) });
		Out.bTurnaround = true;
	}

	/** Samples of an arc about Center from Center + Start, Sweep radians (counter-clockwise positive), and their headings. */
	void AddArc(TArray<TPair<FVector2D, FVector2D>>& Out, const FVector2D& Center, const FVector2D& Start, double Sweep)
	{
		const int32 Steps = FMath::Max(2, FMath::CeilToInt32(FMath::Abs(Sweep) * Start.Size() / FMKIRoadAutoPaths::PointStepCm));
		for (int32 Step = Out.Num() > 0 ? 1 : 0; Step <= Steps; ++Step)
		{
			const FVector2D Arm = Start.GetRotated(FMath::RadiansToDegrees(Sweep * Step / Steps));
			const FVector2D Heading = (Sweep > 0.0 ? FVector2D(-Arm.Y, Arm.X) : FVector2D(Arm.Y, -Arm.X)).GetSafeNormal();
			Out.Add({ Center + Arm, Heading });
		}
	}

	/** Samples of a straight line from From to To. */
	void AddLine(TArray<TPair<FVector2D, FVector2D>>& Out, const FVector2D& From, const FVector2D& To)
	{
		const FVector2D Heading = (To - From).GetSafeNormal();
		// No length (the lane veers right at the arm end): only its start, when nothing precedes it.
		if ((To - From).Size() < 1.0) { if (Out.Num() == 0) Out.Add({ From, FVector2D(Heading.IsNearlyZero() ? FVector2D(-1.0, 0.0) : Heading) }); return; }
		const int32 Steps = FMath::Max(1, FMath::CeilToInt32((To - From).Size() / FMKIRoadAutoPaths::PointStepCm));
		for (int32 Step = Out.Num() > 0 ? 1 : 0; Step <= Steps; ++Step) Out.Add({ From + (To - From) * (static_cast<double>(Step) / Steps), Heading });
	}
	/** A truck depot's deck (#237, #240): its lanes and its bays, whole 8 m tiles along its length; arms at both ends. */
	void TruckDepotCells(FMKIRoadJunctionLayout& Out, int32 Lanes, int32 Arms)
	{
		const float Tile = AMKIRoadSplineRoad::LaneWidthCm, Half = Lanes * Tile * 0.5f, Reach = AMKIRoadJunction::DepotHalfLengthCm;
		for (float X0 = -Reach; X0 < Reach - 1.f; X0 += Tile)
		{
			for (int32 Row = AMKIRoadJunction::IsBothSidesShape(Arms) ? -1 : 0; Row < Lanes + 1; ++Row)
			{
				const float Y0 = -Half + Row * Tile;
				Out.Cells.Add({ FVector2D(X0, Y0), FVector2D(X0 + Tile, Y0), FVector2D(X0 + Tile, Y0 + Tile), FVector2D(X0, Y0 + Tile) });
				Out.TileOrigins.Add(FVector2D(X0, Y0));
			}
		}
		// The pads (#241): asphalt under each station's footprint, beside its dock lane or bay, in 8 m tiles (the last row 4 m).
		TArray<FMKIRoadDepotDock> Docks;
		AMKIRoadJunction::DepotDocks(Arms, Lanes, Docks);
		for (const FMKIRoadDepotDock& Dock : Docks)
		{
			const TArray<FVector2D> Corners = AMKIRoadJunction::DepotPadCorners(Dock);
			const FBox2D Pad(Corners);
			for (double X0 = Pad.Min.X; X0 < Pad.Max.X - 1.0; X0 += Tile)
			{
				for (double Y0 = Pad.Min.Y; Y0 < Pad.Max.Y - 1.0; Y0 += Tile)
				{
					const double X1 = FMath::Min(X0 + Tile, Pad.Max.X), Y1 = FMath::Min(Y0 + Tile, Pad.Max.Y);
					Out.Cells.Add({ FVector2D(X0, Y0), FVector2D(X1, Y0), FVector2D(X1, Y1), FVector2D(X0, Y1) });
					Out.TileOrigins.Add(FVector2D(X0, Y0));
					Out.CellIsPad.SetNum(Out.Cells.Num() - 1, false);
					Out.CellIsPad.Add(true);
				}
			}
		}
		Out.Arms.Add({ FVector2D(Reach, 0.0), FVector2D(1.0, 0.0) });
		Out.Arms.Add({ FVector2D(-Reach, 0.0), FVector2D(-1.0, 0.0) });
		Out.bTruckDepot = true;
	}
	/** The legend glyph for Letter (MKIRoadLegendGlyphs.generated.h), or null. */
	const FMKIRoadLegendGlyph* LegendGlyph(TCHAR Letter)
	{
		for (const FMKIRoadLegendGlyph& Glyph : GMKIRoadLegendGlyphs) if (Glyph.Letter == Letter) return &Glyph;
		return nullptr;
	}

	/** A word's width in cap heights: the glyphs' advances. */
	float LegendWidth(const TCHAR* Word)
	{
		float Width = 0.f;
		for (const TCHAR* Letter = Word; *Letter; ++Letter) if (const FMKIRoadLegendGlyph* Glyph = LegendGlyph(*Letter)) Width += Glyph->Advance;
		return Width;
	}

	FVector2D Centroid(const TArray<FVector2D>& Cell)
	{
		FVector2D Sum = FVector2D::ZeroVector;
		for (const FVector2D& Point : Cell) Sum += Point;
		return Cell.Num() > 0 ? Sum / Cell.Num() : Sum;
	}
}

AMKIRoadJunction::AMKIRoadJunction(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	mDeck = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Deck"));
	mDeck->SetupAttachment(RootComponent);
	// Rendering only, as the road's: holograms and vehicles hit the static mesh collision boxes.
	mDeck->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mDeck->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	mDeckMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/FactoryGame/Buildable/Building/Foundation/AsphaltSet/MI_AsphaltFoundation.MI_AsphaltFoundation")));

	mPad = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Pad"));
	mPad->SetupAttachment(RootComponent);
	mPad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPad->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	mPadStripes = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("PadStripes"));
	mPadStripes->SetupAttachment(RootComponent);
	mPadStripes->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mPadStripes->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	mPadStripes->SetCastShadow(false);

	mMarkings = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Markings"));
	mMarkings->SetupAttachment(RootComponent);
	mMarkings->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	mMarkings->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	mMarkings->SetCastShadow(false);

	mHologramClass = AMKIRoadJunctionHologram::StaticClass();
	mCanContainLightweightInstances = false;
	mManagedByLightweightBuildableSubsystem = false;

	// A foundation, as the road is, so vehicles can be placed on it; no foundation snaps to its edges or faces.
	mWidth = mDepth = 2.f * ApronCm + AMKIRoadSplineRoad::LaneWidthCm * AMKIRoadSplineRoad::DefaultLanes;
	mHeight = mDeckThicknessCm;
	for (FFoundationSideSelectionFlags* Flags : { &mDisableSnapOn, &mDisableAttachmentSnapOn })
	{
		Flags->Front = Flags->Right = Flags->Back = Flags->Left = Flags->Top = Flags->Bottom = true;
	}
}

FMKIRoadJunctionLayout AMKIRoadJunction::Layout(int32 Arms, int32 Lanes)
{
	FMKIRoadJunctionLayout Out;
	const int32 Count = IsSupportedLaneCount(Lanes) ? Lanes : AMKIRoadSplineRoad::DefaultLanes;
	const float Tile = AMKIRoadSplineRoad::LaneWidthCm, Half = Count * Tile * 0.5f, Reach = Half + ApronCm;
	auto Square = [&Out, Tile](float X0, float Y0)
	{
		Out.Cells.Add({ FVector2D(X0, Y0), FVector2D(X0 + Tile, Y0), FVector2D(X0 + Tile, Y0 + Tile), FVector2D(X0, Y0 + Tile) });
		Out.TileOrigins.Add(FVector2D(X0, Y0));
	};
	if (IsTurnaroundShape(Arms)) TurnaroundCells(Out, Count);
	else if (IsDepotShape(Arms)) TruckDepotCells(Out, Count, Arms);
	else
	{
		// The square where the roads cross: Lanes x Lanes tiles.
		for (int32 Row = 0; Row < Count; ++Row) for (int32 Column = 0; Column < Count; ++Column) Square(-Half + Column * Tile, -Half + Row * Tile);
		// The arms: +X and -X (the through road), -Y (the stem), and +Y on a 4-way. Each an apron one tile deep.
		// A 2-way (#228) is a corner: +X and the stem only.
		TArray<FVector2D> Directions = { FVector2D(1.0, 0.0) };
		if (Arms >= 3) Directions.Add(FVector2D(-1.0, 0.0));
		Directions.Add(FVector2D(0.0, -1.0));
		if (Arms >= 4) Directions.Add(FVector2D(0.0, 1.0));
		for (const FVector2D& Direction : Directions)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const float Across = -Half + Index * Tile;
				if (Direction.X > 0.5) Square(Half, Across);
				else if (Direction.X < -0.5) Square(-Reach, Across);
				else if (Direction.Y < -0.5) Square(Across, -Reach);
				else Square(Across, Half);
			}
			Out.Arms.Add({ Direction * Reach, Direction });
		}
		// The corners between two neighbouring arms: the whole tile (#314; the maintainer: every foundation makes squares, and the
		// rounded corners of #230 left gaps nothing could fill). The curb return is painted across it (MarkingGeometry); the
		// right turn's path stays inside the tile, which is all the vanilla path needs under it.
		for (const FVector2D& A : Directions)
		{
			for (const FVector2D& B : Directions)
			{
				// Each corner once: A along X, B along Y.
				if (FMath::Abs(A.X) < 0.5 || FMath::Abs(B.Y) < 0.5) continue;
				const float SX = static_cast<float>(A.X), SY = static_cast<float>(B.Y);
				Square(FMath::Min(SX * Half, SX * Reach), FMath::Min(SY * Half, SY * Reach));
				Out.CellIsCorner.SetNum(Out.Cells.Num() - 1, false);
				Out.CellIsCorner.Add(true);
			}
		}
	}
	Out.CellIsCorner.SetNum(Out.Cells.Num(), false);
	// The outline: every cell edge no other cell shares.
	TMap<TTuple<int32, int32, int32, int32>, int32> Uses;
	for (const TArray<FVector2D>& Cell : Out.Cells) for (int32 Index = 0; Index < Cell.Num(); ++Index) Uses.FindOrAdd(EdgeKey(Cell[Index], Cell[(Index + 1) % Cell.Num()]))++;
	for (int32 CellIndex = 0; CellIndex < Out.Cells.Num(); ++CellIndex)
	{
		const TArray<FVector2D>& Cell = Out.Cells[CellIndex];
		for (int32 Index = 0; Index < Cell.Num(); ++Index)
		{
			const FVector2D P = Cell[Index], Q = Cell[(Index + 1) % Cell.Num()];
			if (Uses.FindRef(EdgeKey(P, Q)) != 1) continue;
			Out.OutlineEdges.Add({ P, Q });
			Out.OutlineCells.Add(CellIndex);
			const FVector2D Middle = (P + Q) * 0.5;
			bool bArmEnd = false;
			// On an arm's end line and within its width: the corner's straight pieces past the width are curb, not arm end.
			for (const FMKIRoadJunctionArm& Arm : Out.Arms) bArmEnd |= FMath::Abs(FVector2D::DotProduct(Middle - Arm.Center, Arm.Outward)) < 1.0 && FMath::Abs(FVector2D::CrossProduct(Q - P, Arm.Outward)) > 1.0 && FMath::Abs(FVector2D::CrossProduct(Middle - Arm.Center, Arm.Outward)) <= Half + 1.0;
			Out.OutlineIsArmEnd.Add(bArmEnd);
		}
	}
	return Out;
}

float AMKIRoadJunction::TurnaroundArmEndCm(int32 Lanes)
{
	float VeerStart = 0.f;
	for (const FTurnLane& Lane : TurnLanes(Lanes)) VeerStart = FMath::Max(VeerStart, Lane.VeerStart);
	return FMath::CeilToFloat(VeerStart / 100.f) * 100.f;
}

float AMKIRoadJunction::TurnaroundDeckRadiusCm(int32 Lanes)
{
	float Loop = 0.f;
	for (const FTurnLane& Lane : TurnLanes(Lanes)) Loop = FMath::Max(Loop, Lane.Loop);
	return Loop + AMKIRoadSplineRoad::LaneWidthCm * 0.5f + 50.f;
}

void AMKIRoadJunction::TurnaroundLoops(int32 Lanes, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutHalves, double SplitFraction, int32 OnlyLane)
{
	const double ArmEnd = TurnaroundArmEndCm(Lanes), Veer = TurnVeerRadiusCm;
	const TArray<FTurnLane> Lanes2 = TurnLanes(Lanes);
	for (int32 LaneIndex = 0; LaneIndex < Lanes2.Num(); ++LaneIndex)
	{
		if (OnlyLane != INDEX_NONE && LaneIndex != OnlyLane) continue;
		const FTurnLane& Lane = Lanes2[LaneIndex];
		// Arriving traffic runs in along -X on the arm's -Y side and leaves along +X on its +Y side (keep right).
		const double L = Lane.Lateral, R = Lane.Loop, X1 = Lane.VeerStart;
		// The right veer: about VeerCenter, from the lane at X1 round to where it touches the loop.
		const FVector2D VeerCenter(X1, -(L + Veer)), FromCenter = -VeerCenter / (R + Veer);
		const FVector2D VeerStart(0.0, Veer), VeerEnd = FromCenter * Veer;
		const double VeerSweep = FMath::Atan2(FVector2D::CrossProduct(VeerStart, VeerEnd), FVector2D::DotProduct(VeerStart, VeerEnd));
		// The loop: clockwise about the center from that point round the far side to its mirror.
		const FVector2D LoopStart = -FromCenter * R;
		const double Alpha = FMath::Abs(FMath::Atan2(LoopStart.Y, LoopStart.X));
		const double LoopSweep = -(2.0 * PI - 2.0 * Alpha);
		TArray<TPair<FVector2D, FVector2D>> In, Back;
		AddLine(In, FVector2D(ArmEnd, -L), FVector2D(X1, -L));
		AddArc(In, VeerCenter, VeerStart, VeerSweep);
		AddArc(In, FVector2D::ZeroVector, LoopStart, LoopSweep * SplitFraction);
		AddArc(Back, FVector2D::ZeroVector, LoopStart.GetRotated(FMath::RadiansToDegrees(LoopSweep * SplitFraction)), LoopSweep * (1.0 - SplitFraction));
		// The mirror veer back into the leaving lane, and out along it.
		const FVector2D MirrorCenter(X1, L + Veer);
		AddArc(Back, MirrorCenter, FVector2D(VeerEnd.X, -VeerEnd.Y), VeerSweep);
		AddLine(Back, FVector2D(X1, L), FVector2D(ArmEnd, L));
		OutHalves.Add(MoveTemp(In));
		OutHalves.Add(MoveTemp(Back));
	}
}

void AMKIRoadJunction::DepotDocks(int32 Arms, int32 Lanes, TArray<FMKIRoadDepotDock>& OutDocks)
{
	const int32 Count = IsSupportedLaneCount(Lanes) ? Lanes : AMKIRoadSplineRoad::DefaultLanes;
	const double Width = AMKIRoadSplineRoad::LaneWidthCm, Half = Count * Width * 0.5;
	// On the bay's center; turned half round, the building is on +Y.
	const double Y = Half + Width * 0.5;
	OutDocks.Add({ FVector2D(0.0, Y), 180.f });
	if (IsBothSidesShape(Arms)) OutDocks.Add({ FVector2D(0.0, -Y), 0.f });
}

TArray<FVector2D> AMKIRoadJunction::DepotPadCorners(const FMKIRoadDepotDock& Dock)
{
	// The truck station's building (Build_TruckStation's clearance: X -6 m to 11 m, Y -13 m to -4 m about its dock),
	// turned with the station. #257 (the maintainer): four by one 8 m foundations, 32 m along the bay and one tile deep from its
	// edge, on the depot's 8 m grid so vanilla foundations extend it (the building's back metre overhangs it).
	const FVector2D Min(-2.0 * AMKIRoadSplineRoad::LaneWidthCm, -400.0 - AMKIRoadSplineRoad::LaneWidthCm), Max(2.0 * AMKIRoadSplineRoad::LaneWidthCm, -400.0);
	TArray<FVector2D> Corners;
	for (const FVector2D& Local : { Min, FVector2D(Max.X, Min.Y), Max, FVector2D(Min.X, Max.Y) }) Corners.Add(Dock.Point + Local.GetRotated(Dock.StationYawDeg));
	return Corners;
}

void AMKIRoadJunction::DepotPaths(int32 Arms, int32 Lanes, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutPaths)
{
	const int32 Count = IsSupportedLaneCount(Lanes) ? Lanes : AMKIRoadSplineRoad::DefaultLanes;
	const double Width = AMKIRoadSplineRoad::LaneWidthCm, Half = Count * Width * 0.5, Reach = DepotHalfLengthCm;
	TArray<FMKIRoadDepotDock> Docks;
	DepotDocks(Arms, Count, Docks);
	// The through lanes, whole.
	StraightLanePaths(Count, static_cast<float>(Reach), OutPaths);
	// Each bay, laid for the +Y one (off the outer +X lane) and turned half round for the -Y one (the outer -X lane): a bend of
	// two arcs from the lane at the depot's end into the bay (the maintainer, #250: the paths end right on the border to the next
	// piece), DockStraightCm straight into the dock and out of it, each its own path (#247: a station left straight into a
	// curve made the truck stutter), and the mirror bend back onto the lane at the other end.
	static_assert(DockStraightCm + TruckBayBendRunCm == DepotHalfLengthCm, "a bay's straight and bend fill half the depot");
	const double Outer = Half - Width * 0.5, Bay = Half + Width * 0.5, Run = TruckBayBendRunCm;
	double R = 0.0, Theta = 0.0;
	BayBendArcs(Run, Bay - Outer, R, Theta);
	const double S = FMath::Sin(Theta), C = FMath::Cos(Theta);
	for (const FMKIRoadDepotDock& Dock : Docks)
	{
		// Live (#250): an empty line before a bend started the path facing back (-X), and the game showed it red; each bend
		// starts its own path.
		TArray<TPair<FVector2D, FVector2D>> In, ToDock, FromDock, Out;
		AddArc(In, FVector2D(-Reach, Outer + R), FVector2D(0.0, -R), Theta);
		AddArc(In, FVector2D(-Reach + Run, Bay - R), FVector2D(-R * S, R * C), -Theta);
		AddLine(ToDock, FVector2D(-DockStraightCm, Bay), FVector2D(0.0, Bay));
		AddLine(FromDock, FVector2D(0.0, Bay), FVector2D(DockStraightCm, Bay));
		AddArc(Out, FVector2D(Reach - Run, Bay - R), FVector2D(0.0, R), -Theta);
		AddArc(Out, FVector2D(Reach, Outer + R), FVector2D(-R * S, -R * C), Theta);
		if (Dock.Point.Y < 0.0) for (TArray<TPair<FVector2D, FVector2D>>* Path : { &In, &ToDock, &FromDock, &Out }) for (TPair<FVector2D, FVector2D>& Point : *Path) { Point.Key = -Point.Key; Point.Value = -Point.Value; }
		OutPaths.Add(MoveTemp(In));
		OutPaths.Add(MoveTemp(ToDock));
		OutPaths.Add(MoveTemp(FromDock));
		OutPaths.Add(MoveTemp(Out));
	}
}

void AMKIRoadJunction::StraightLanePaths(int32 Lanes, float HalfLengthCm, TArray<TArray<TPair<FVector2D, FVector2D>>>& OutPaths)
{
	const int32 Count = IsSupportedLaneCount(Lanes) ? Lanes : AMKIRoadSplineRoad::DefaultLanes;
	const float Width = AMKIRoadSplineRoad::LaneWidthCm, Half = Count * Width * 0.5f;
	for (int32 Lane = 0; Lane < Count; ++Lane)
	{
		const float Lateral = -Half + (Lane + 0.5f) * Width;
		// As a road's (FMKIRoadAutoPaths::RoadPaths): right of the centerline runs +X, left of it -X.
		if (Lateral > 0.f) { TArray<TPair<FVector2D, FVector2D>> Path; AddLine(Path, FVector2D(-HalfLengthCm, Lateral), FVector2D(HalfLengthCm, Lateral)); OutPaths.Add(MoveTemp(Path)); }
		else { TArray<TPair<FVector2D, FVector2D>> Path; AddLine(Path, FVector2D(HalfLengthCm, Lateral), FVector2D(-HalfLengthCm, Lateral)); OutPaths.Add(MoveTemp(Path)); }
	}
}

FBox2D AMKIRoadJunction::LayoutBounds(const FMKIRoadJunctionLayout& Layout)
{
	FBox2D Box(ForceInit);
	for (const TArray<FVector2D>& Cell : Layout.Cells) for (const FVector2D& Point : Cell) Box += Point;
	return Box;
}

double AMKIRoadJunction::LayoutAreaCm2(const FMKIRoadJunctionLayout& Layout)
{
	double Area = 0.0;
	for (const TArray<FVector2D>& Cell : Layout.Cells)
	{
		double Twice = 0.0;
		for (int32 Index = 0; Index < Cell.Num(); ++Index) Twice += FVector2D::CrossProduct(Cell[Index], Cell[(Index + 1) % Cell.Num()]);
		Area += FMath::Abs(Twice) * 0.5;
	}
	return Area;
}

int32 AMKIRoadJunction::FoundationEquivalents(int32 Arms, int32 Lanes)
{
	return FMath::Max(1, FMath::CeilToInt32(LayoutAreaCm2(Layout(Arms, Lanes)) / AMKIRoadSplineRoad::FoundationAreaCm2 - 1e-6));
}

FMKIRoadDeckGeometry AMKIRoadJunction::BuildGeometry(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, int32 Part)
{
	FMKIRoadDeckGeometry Out;
	const float Tile = AMKIRoadSplineRoad::UvTileCm;
	// As AMKIRoadSplineRoad::SweepDeck: a procedural mesh triangle faces away from (B - A) x (C - A); each is ordered to face out.
	auto AddTriangle = [&Out](int32 A, int32 B, int32 C, const FVector& Normal)
	{
		const FVector Face = FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[A], Out.Vertices[C] - Out.Vertices[A]);
		if (FVector::DotProduct(Face, Normal) > 0.f) Swap(B, C);
		Out.Triangles.Append({ A, B, C });
	};
	auto AddVertex = [&Out](const FVector& Position, const FVector& Normal, const FVector2D& UV, const FVector2D& UV1, const FVector& Tangent)
	{
		Out.Vertices.Add(Position); Out.Normals.Add(Normal); Out.UVs.Add(UV); Out.UV1s.Add(UV1); Out.Tangents.Add(Tangent);
		return Out.Vertices.Num() - 1;
	};
	constexpr float TopSpan = 0.974f, SideSpan = 0.978f;
	// Top and bottom, per cell, with its tile's UVs: U across (local +Y) and V back on the top, as the road's tiles.
	for (int32 CellIndex = 0; CellIndex < Layout.Cells.Num(); ++CellIndex)
	{
		const TArray<FVector2D>& Cell = Layout.Cells[CellIndex];
		const FVector2D Origin = Layout.TileOrigins[CellIndex];
		// Part 0: the deck without its pads; 1: the pads alone (#246); otherwise all of it.
		const bool bPad = Layout.CellIsPad.IsValidIndex(CellIndex) && Layout.CellIsPad[CellIndex];
		if ((Part == 0 && bPad) || (Part == 1 && !bPad)) continue;
		for (const bool bTop : { true, false })
		{
			const FVector Normal = bTop ? FVector::UpVector : -FVector::UpVector;
			const float Z = bTop ? 0.f : -ThicknessCm;
			const int32 First = Out.Vertices.Num();
			for (const FVector2D& Point : Cell)
			{
				const float A = static_cast<float>((Point.X - Origin.X) / Tile), C = static_cast<float>((Point.Y - Origin.Y) / Tile);
				const FVector2D UV = bTop ? FVector2D((C - 0.5f) * TopSpan, (0.5f - A) * TopSpan) : FVector2D(2.f + (0.5f - A) * TopSpan, (C - 0.5f) * TopSpan);
				AddVertex(FVector(Point.X, Point.Y, Z), Normal, UV, FVector2D(A, C), FVector::ForwardVector);
			}
			for (int32 Index = 1; Index + 1 < Cell.Num(); ++Index) AddTriangle(First, First + Index, First + Index + 1, Normal);
		}
	}
	// Sides on the outline: the side band hung from the deck top, as the road's sides and end caps (with the deck, not a pad).
	for (int32 EdgeIndex = 0; Part != 1 && EdgeIndex < Layout.OutlineEdges.Num(); ++EdgeIndex)
	{
		const FVector2D P = Layout.OutlineEdges[EdgeIndex].Key, Q = Layout.OutlineEdges[EdgeIndex].Value;
		const FVector2D Out2D = OutwardNormal(Layout.Cells[Layout.OutlineCells[EdgeIndex]], P, Q);
		const FVector Normal(Out2D.X, Out2D.Y, 0.f), Along = FVector(Q.X - P.X, Q.Y - P.Y, 0.f).GetSafeNormal();
		const float Bottom = 1.f - ThicknessCm / AMKIRoadSplineRoad::SideBandCm;
		const int32 A = AddVertex(FVector(P.X, P.Y, 0.f), Normal, FVector2D(2.f - 0.5f * SideSpan, 1.f), FVector2D(0.f, 0.f), Along);
		const int32 B = AddVertex(FVector(Q.X, Q.Y, 0.f), Normal, FVector2D(2.f + 0.5f * SideSpan, 1.f), FVector2D(1.f, 0.f), Along);
		const int32 C = AddVertex(FVector(P.X, P.Y, -ThicknessCm), Normal, FVector2D(2.f - 0.5f * SideSpan, Bottom), FVector2D(0.f, 0.f), Along);
		const int32 D = AddVertex(FVector(Q.X, Q.Y, -ThicknessCm), Normal, FVector2D(2.f + 0.5f * SideSpan, Bottom), FVector2D(1.f, 0.f), Along);
		AddTriangle(A, B, C, Normal);
		AddTriangle(B, D, C, Normal);
	}
	return Out;
}

void AMKIRoadJunction::CollisionBoxes(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<FTransform>& OutBoxes)
{
	// Each box: its center (half the thickness below the top) and full size as the scale. Every cell is a rectangle (#314:
	// the rounded corners and the round turnaround, which needed strips, are whole tiles now).
	for (const TArray<FVector2D>& Cell : Layout.Cells)
	{
		if (Cell.Num() != 4) continue;
		const FVector2D Middle = Centroid(Cell);
		const FVector2D Size = Cell[2] - Cell[0];
		OutBoxes.Add(FTransform(FQuat::Identity, FVector(Middle.X, Middle.Y, -ThicknessCm * 0.5f), FVector(FMath::Abs(Size.X), FMath::Abs(Size.Y), ThicknessCm)));
	}
}

void AMKIRoadJunction::MakeClearanceData(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<FFGClearanceData>& Out)
{
	const float Inset = AMKIRoadSplineRoad::ClearanceInsetCm, TopInset = FMath::Min(AMKIRoadSplineRoad::ClearanceTopInsetCm, ThicknessCm * 0.5f);
	for (const TArray<FVector2D>& Cell : Layout.Cells)
	{
		// Whole tiles only (a turnaround's cut tiles, #236, may have four corners too).
		if (!IsWholeTile(Cell)) continue;
		const FBox2D Box(Cell);
		const FVector2D Min = Box.Min, Max = Box.Max;
		FFGClearanceData Data;
		Data.Type = EClearanceType::CT_Soft;
		Data.ClearanceBox = FBox(FVector(Min.X + Inset, Min.Y + Inset, -ThicknessCm + Inset), FVector(Max.X - Inset, Max.Y - Inset, -TopInset));
		Out.Add(Data);
	}
}

void AMKIRoadJunction::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AMKIRoadJunction, mLanes);
}

void AMKIRoadJunction::SetLanes(int32 Lanes)
{
	if (IsSupportedLaneCount(Lanes)) mLanes = Lanes;
}

void AMKIRoadJunction::BeginPlay()
{
	if (!IsSupportedLaneCount(mLanes)) mLanes = AMKIRoadSplineRoad::DefaultLanes;
	if (mBayPathsLaid) { mDocksJoined |= 1; mBayPathsLaid = false; }
	const FVector2D Size = LayoutBounds(Layout(mArms, mLanes)).GetSize();
	mWidth = mDepth = static_cast<float>(FMath::Max(Size.X, Size.Y));
	mHeight = mDeckThicknessCm;
	BuildDeck();
	BuildPadStripes();
	ResetClearanceData();
	TArray<FFGClearanceData> Clearance;
	MakeClearanceData(Layout(mArms, mLanes), mDeckThicknessCm, Clearance);
	AddClearanceData(Clearance);
	Super::BeginPlay();
	if (UWorld* World = GetWorld())
	{
		// Its squares carry the collision and the snapping: next tick, once a loaded save has resolved the saved ones.
		if (HasAuthority()) World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { SyncTiles(); }));
		// Its vehicle paths (#235), once, after the roads loaded with it have begun play.
		if (HasAuthority() && mAutoPathsVersion < FMKIRoadAutoPaths::Version)
		{
			// Paths laid by an older version go first, so none is left when any road or intersection lays its new paths (live:
			// one laid first joined a neighbour's old node, which went with the old path, and the neighbour made a second node).
			// Removed as the player's dismantle does, next tick (the vehicle subsystem ready), and laid the tick after, when every
			// road and intersection has removed its own. Live (#239): Destroy() left the path nodes of other pieces and stations
			// linked to the gone paths, and dismantling one of those later left a save that crashed the vehicle subsystem on load.
			World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				for (AFGVehiclePathSegment* Old : mAutoPaths) if (IsValid(Old) && !Old->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Old);
				mAutoPaths.Reset();
				// A depot's old bay paths too.
				for (AFGVehiclePathSegment* Old : mBayPaths) if (IsValid(Old) && !Old->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Old);
				mBayPaths.Reset();
				mBayPathsLaid = false;
				mDocksJoined = 0;
				FMKIRoadAutoPaths::CleanStaleNodeLinks(GetWorld());
				GetWorld()->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [this]() { LayAutoPaths(); }));
			}));
		}
	}
}

bool AMKIRoadJunction::OwnsAutoPath(const AActor* Path) const
{
	auto Is = [Path](const TObjectPtr<AFGVehiclePathSegment>& Own) { return Own.Get() == Path; };
	return Path && (mAutoPaths.ContainsByPredicate(Is) || mBayPaths.ContainsByPredicate(Is));
}

void AMKIRoadJunction::LayAutoPaths()
{
	if (mAutoPathsVersion >= FMKIRoadAutoPaths::Version || IsActorBeingDestroyed()) return;
	TArray<TArray<FSplinePointData>> Paths;
	FMKIRoadAutoPaths::JunctionPaths(*this, Paths);
	TArray<AFGVehiclePathSegment*> Segments;
	FMKIRoadAutoPaths::Spawn(GetWorld(), Paths, Segments);
	for (AFGVehiclePathSegment* Segment : Segments) mAutoPaths.Add(Segment);
	mAutoPathsLaid = true;
	mAutoPathsVersion = FMKIRoadAutoPaths::Version;
	LayBayPaths();
}

TArray<FTransform> AMKIRoadJunction::GetDepotDocks() const
{
	TArray<FTransform> Out;
	TArray<FMKIRoadDepotDock> Docks;
	DepotDocks(mArms, mLanes, Docks);
	for (const FMKIRoadDepotDock& Dock : Docks)
	{
		Out.Add(FTransform(FRotator(0.f, GetActorRotation().Yaw + Dock.StationYawDeg, 0.f), GetActorTransform().TransformPosition(FVector(Dock.Point.X, Dock.Point.Y, 0.0))));
	}
	return Out;
}

int32 AMKIRoadJunction::FindDepotDock(const FVector& Location, float MaxDistanceCm) const
{
	int32 Best = INDEX_NONE;
	float BestDistance = MaxDistanceCm;
	const TArray<FTransform> Docks = GetDepotDocks();
	for (int32 Index = 0; Index < Docks.Num(); ++Index)
	{
		const float Distance = static_cast<float>(FVector::Dist2D(Docks[Index].GetLocation(), Location));
		if (Distance <= BestDistance) { Best = Index; BestDistance = Distance; }
	}
	return Best;
}

void AMKIRoadJunction::LayBayPaths()
{
	if (!IsTruckDepot() || !HasAuthority() || IsActorBeingDestroyed()) return;
	// Its lanes first: the dock paths meet at the docking points.
	if (mAutoPathsVersion < FMKIRoadAutoPaths::Version) return;
	const TArray<FTransform> Docks = GetDepotDocks();
	for (int32 Index = 0; Index < Docks.Num() && Index < 31; ++Index)
	{
		// Once per dock (the maintainer: paths he deleted came back).
		if (mDocksJoined & (1 << Index)) continue;
		const FVector Dock = Docks[Index].GetLocation();
		// A station on the dock (the snap puts it there), and its docking node there. Live (#239), a loaded station's
		// GetDockingPathNode was empty, so the node is also found by place and class.
		AFGVehiclePathNode* DockNode = nullptr;
		bool bStation = false;
		for (TActorIterator<AFGBuildableDockingStation> It(GetWorld()); It && !bStation; ++It)
		{
			if (FVector::Dist(It->GetActorLocation(), Dock) > 2.f * FMKIRoadAutoPaths::JoinRadiusCm) continue;
			bStation = true;
			DockNode = It->GetDockingPathNode();
		}
		if (!bStation) continue;
		bool bOurs = false;
		for (TActorIterator<AFGVehiclePathNode> It(GetWorld()); It; ++It)
		{
			if (!IsValid(*It) || It->IsActorBeingDestroyed() || FVector::Dist(It->GetActorLocation(), Dock) > FMKIRoadAutoPaths::JoinRadiusCm) continue;
			if (It->GetClass()->GetName().Contains(TEXT("DockingStation"))) { if (!DockNode) DockNode = *It; }
			else bOurs = true;
		}
		if (!DockNode)
		{
			UE_LOG(LogMKIRoads, Verbose, TEXT("Truck depot %s: a station stands on dock %d, its docking node not yet"), *GetName(), Index);
			continue;
		}
		// The dock's paths are laid again, now ending on the station's node. Live (#239): handing the middle node's
		// connections to the station's (MoveConnectionsToNode) left a save that crashed the vehicle subsystem on load.
		if (bOurs) RelayDockLane(Index);
		mDocksJoined |= 1 << Index;
		// Joined when paths arrive at and leave from the station's own node (the maintainer: make sure the paths connect to it).
		const bool bJoined = DockNode->GetArrivingConnections().Num() > 0 && DockNode->GetLeavingConnections().Num() > 0;
		UE_LOG(LogMKIRoads, Log, TEXT("Truck depot %s: dock %d through %s (%d in, %d out): %s"), *GetName(), Index, *DockNode->GetName(), DockNode->GetArrivingConnections().Num(), DockNode->GetLeavingConnections().Num(), bJoined ? TEXT("joined") : TEXT("NOT joined"));
	}
}

void AMKIRoadJunction::OnDockStationRemovedAt(const FVector& Location)
{
	if (!IsTruckDepot() || !HasAuthority() || IsActorBeingDestroyed()) return;
	const int32 Dock = FindDepotDock(Location, 200.f);
	if (Dock == INDEX_NONE) return;
	RelayDockLane(Dock);
	if (Dock < 31) mDocksJoined &= ~(1 << Dock);
}

void AMKIRoadJunction::RelayDockLane(int32 DockIndex)
{
	const TArray<FTransform> Docks = GetDepotDocks();
	if (!Docks.IsValidIndex(DockIndex)) return;
	const FVector Dock = Docks[DockIndex].GetLocation();
	auto TouchesDock = [&Dock](const AFGVehiclePathSegment* Segment)
	{
		const USplineComponent* Spline = Segment->GetSplineComponent();
		if (!Spline || Spline->GetNumberOfSplinePoints() < 2) return false;
		const FVector Start = Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World), End = Spline->GetLocationAtSplinePoint(Spline->GetNumberOfSplinePoints() - 1, ESplineCoordinateSpace::World);
		return FVector::Dist(Start, Dock) <= FMKIRoadAutoPaths::JoinRadiusCm || FVector::Dist(End, Dock) <= FMKIRoadAutoPaths::JoinRadiusCm;
	};
	int32 Removed = 0;
	for (int32 Index = mAutoPaths.Num() - 1; Index >= 0; --Index)
	{
		AFGVehiclePathSegment* Segment = mAutoPaths[Index];
		if (!IsValid(Segment) || Segment->IsActorBeingDestroyed()) { mAutoPaths.RemoveAt(Index); continue; }
		if (!TouchesDock(Segment)) continue;
		// As the player's dismantle does (live, #239: Destroy() in play left the vehicle network holding the gone segment, and
		// the save crashed the vehicle subsystem on load).
		IFGDismantleInterface::Execute_Dismantle(Segment);
		mAutoPaths.RemoveAt(Index);
		++Removed;
	}
	// The dock's own middle node goes too, so the paths end on the station's node when there is one.
	for (TActorIterator<AFGVehiclePathNode> It(GetWorld()); It; ++It)
	{
		if (IsValid(*It) && !It->IsActorBeingDestroyed() && !It->GetClass()->GetName().Contains(TEXT("DockingStation")) && FVector::Dist(It->GetActorLocation(), Dock) <= FMKIRoadAutoPaths::JoinRadiusCm) IFGDismantleInterface::Execute_Dismantle(*It);
	}
	FMKIRoadAutoPaths::CleanStaleNodeLinks(GetWorld());
	TArray<TArray<FSplinePointData>> Paths;
	FMKIRoadAutoPaths::DepotDockPaths(*this, DockIndex, Paths);
	TArray<AFGVehiclePathSegment*> Segments;
	FMKIRoadAutoPaths::Spawn(GetWorld(), Paths, Segments);
	for (AFGVehiclePathSegment* Segment : Segments) mAutoPaths.Add(Segment);
	UE_LOG(LogMKIRoads, Log, TEXT("Truck depot %s: dock %d paths laid again (%d removed, %d laid)"), *GetName(), DockIndex, Removed, Segments.Num());
}

void AMKIRoadJunction::Dismantle_Implementation()
{
	if (HasAuthority())
	{
		for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path) && !Path->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Path);
		mAutoPaths.Reset();
		for (AFGVehiclePathSegment* Path : mBayPaths) if (IsValid(Path) && !Path->IsActorBeingDestroyed()) IFGDismantleInterface::Execute_Dismantle(Path);
		mBayPaths.Reset();
		// Its squares, if it goes on its own (the bridge, or a dismantle that did not gather them).
		for (AMKIRoadTile* Tile : mTiles) if (IsValid(Tile) && !Tile->IsActorBeingDestroyed()) Tile->Destroy();
		mTiles.Reset();
	}
	Super::Dismantle_Implementation();
}

void AMKIRoadJunction::StartIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	Super::StartIsLookedAtForDismantle_Implementation(ByCharacter);
	for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(true);
	for (AFGVehiclePathSegment* Path : mBayPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(true);
}

void AMKIRoadJunction::StopIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* ByCharacter)
{
	Super::StopIsLookedAtForDismantle_Implementation(ByCharacter);
	for (AFGVehiclePathSegment* Path : mAutoPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(false);
	for (AFGVehiclePathSegment* Path : mBayPaths) if (IsValid(Path)) Path->TogglePendingDismantleMaterial(false);
}

void AMKIRoadJunction::GetChildDismantleActors_Implementation(TArray<AActor*>& OutChildDismantleActors) const
{
	Super::GetChildDismantleActors_Implementation(OutChildDismantleActors);
	// Its vehicle paths (#235) are not children: it removes them itself (Dismantle_Implementation, #244).
	// Its supports are (#320), as a road's are.
	for (TActorIterator<AMKIRoadSupports> It(GetWorld()); It; ++It) if (It->GetJunction() == this) OutChildDismantleActors.AddUnique(*It);
	// Its squares go with it (the maintainer, 2026-10-18: a piece is dismantled whole, not square by square).
	for (AMKIRoadTile* Tile : mTiles) if (IsValid(Tile) && !Tile->IsActorBeingDestroyed()) OutChildDismantleActors.AddUnique(Tile);
}

AMKIRoadJunction* AMKIRoadJunction::FromActor(AActor* Actor)
{
	if (AMKIRoadJunction* Junction = Cast<AMKIRoadJunction>(Actor)) return Junction;
	const AMKIRoadTile* Tile = Cast<AMKIRoadTile>(Actor);
	return Tile ? Tile->GetJunction() : nullptr;
}

void AMKIRoadJunction::AddSelfAndTiles(TSet<AActor*>& Actors) const
{
	Actors.Add(const_cast<AMKIRoadJunction*>(this));
	for (AMKIRoadTile* Tile : mTiles) if (IsValid(Tile)) Actors.Add(Tile);
}

void AMKIRoadJunction::TileBoxes(const FMKIRoadJunctionLayout& Layout, float ThicknessCm, TArray<TPair<FVector, FVector>>& OutTiles)
{
	// Each cell (a rectangle) split into whole foundation squares; a cell off the 8 m grid keeps its own size per row.
	const double Tile = AMKIRoadSplineRoad::LaneWidthCm;
	TArray<FTransform> Boxes;
	CollisionBoxes(Layout, ThicknessCm, Boxes);
	for (const FTransform& Box : Boxes)
	{
		const FVector Size = Box.GetScale3D();
		const int32 CountX = FMath::Max(1, FMath::RoundToInt32(Size.X / Tile)), CountY = FMath::Max(1, FMath::RoundToInt32(Size.Y / Tile));
		const FVector Piece(Size.X / CountX, Size.Y / CountY, Size.Z);
		for (int32 X = 0; X < CountX; ++X) for (int32 Y = 0; Y < CountY; ++Y)
		{
			const FVector Middle = Box.GetLocation() + FVector((X + 0.5) * Piece.X - Size.X * 0.5, (Y + 0.5) * Piece.Y - Size.Y * 0.5, 0.0);
			OutTiles.Add(TPair<FVector, FVector>(Middle, Piece));
		}
	}
}

void AMKIRoadJunction::SyncTiles()
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || IsActorBeingDestroyed()) return;
	TArray<TPair<FVector, FVector>> Wanted;
	TileBoxes(Layout(mArms, mLanes), mDeckThicknessCm, Wanted);
	const FTransform Frame = GetActorTransform();
	bool bKeep = mTiles.Num() == Wanted.Num();
	for (const AMKIRoadTile* Tile : mTiles) bKeep &= IsValid(Tile) && !Tile->IsActorBeingDestroyed() && Tile->GetJunction() == this;
	if (bKeep)
	{
		// The saved squares, put back where the layout has them.
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			mTiles[Index]->Setup(this, Wanted[Index].Value);
			mTiles[Index]->SetActorTransform(FTransform(Frame.GetRotation(), Frame.TransformPosition(Wanted[Index].Key)));
		}
		return;
	}
	for (AMKIRoadTile* Tile : mTiles) if (IsValid(Tile) && !Tile->IsActorBeingDestroyed()) Tile->Destroy();
	mTiles.Reset();
	for (const TPair<FVector, FVector>& Box : Wanted)
	{
		const FTransform Placed(Frame.GetRotation(), Frame.TransformPosition(Box.Key));
		AMKIRoadTile* Tile = World->SpawnActorDeferred<AMKIRoadTile>(AMKIRoadTile::StaticClass(), Placed, this, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Tile) continue;
		Tile->Setup(this, Box.Value);
		Tile->FinishSpawning(Placed);
		mTiles.Add(Tile);
	}
}

void AMKIRoadJunction::BuildDeck()
{
	mDeck->ClearAllMeshSections();
	const FMKIRoadJunctionLayout Shape = Layout(mArms, mLanes);
	const bool bPads = Shape.CellIsPad.Contains(true);
	if (mPad)
	{
		mPad->ClearAllMeshSections();
		if (bPads)
		{
			const FMKIRoadDeckGeometry Pad = BuildGeometry(Shape, mDeckThicknessCm, 1);
			TArray<FProcMeshTangent> PadTangents;
			for (const FVector& Tangent : Pad.Tangents) PadTangents.Add(FProcMeshTangent(Tangent, false));
			mPad->CreateMeshSection_LinearColor(0, Pad.Vertices, Pad.Triangles, Pad.Normals, Pad.UVs, Pad.UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), PadTangents, true);
			if (UMaterialInterface* Material = mDeckMaterial.LoadSynchronous()) mPad->SetMaterial(0, Material);
		}
	}
	const FMKIRoadDeckGeometry Geometry = BuildGeometry(Shape, mDeckThicknessCm, bPads ? 0 : -1);
	TArray<FProcMeshTangent> Tangents;
	for (const FVector& Tangent : Geometry.Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
	mDeck->CreateMeshSection_LinearColor(0, Geometry.Vertices, Geometry.Triangles, Geometry.Normals, Geometry.UVs, Geometry.UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, true);
	if (UMaterialInterface* Material = mDeckMaterial.LoadSynchronous()) mDeck->SetMaterial(0, Material);
}

int32 AMKIRoadJunction::GetCollisionBoxCount() const
{
	// Its collision is its squares' now.
	return mTiles.Num();
}

void AMKIRoadJunction::ApplyCustomizationData_Native(const FFactoryCustomizationData& InCustomizationData)
{
	// As the road (#190): a vanilla pattern is refused, the road marking pattern kept; a remover clears it.
	FFactoryCustomizationData CustomizationData = InCustomizationData;
	if (AMKIRoadSplineRoad::IsVanillaPattern(CustomizationData.PatternDesc))
	{
		CustomizationData.PatternDesc = mRoadPattern;
		CustomizationData.PatternRotation = mRoadPatternRotation;
		GetCustomizationData_Native().PatternDesc = mRoadPattern;
		GetCustomizationData_Native().PatternRotation = mRoadPatternRotation;
	}
	else
	{
		mRoadPattern = CustomizationData.PatternDesc && CustomizationData.PatternDesc->IsChildOf(UMKIRoadMarkingPattern::StaticClass()) ? CustomizationData.PatternDesc : nullptr;
		mRoadPatternRotation = CustomizationData.PatternRotation;
	}
	Super::ApplyCustomizationData_Native(CustomizationData);
	const UMKIRoadMarkingPattern* Pattern = CustomizationData.PatternDesc ? Cast<UMKIRoadMarkingPattern>(CustomizationData.PatternDesc->GetDefaultObject()) : nullptr;
	FResolvedFactoryCustomizationData Resolved(CustomizationData);
	if (Pattern) Resolved.PatternID = 0.f;
	if (mDeck) Resolved.ApplyToMeshComponent(mDeck);
	// The pads take the deck's colour; their factory-zone stripes are painted (BuildPadStripes, #246).
	if (mPad) Resolved.ApplyToMeshComponent(mPad);
	BuildPadStripes();
	BuildMarkings(Pattern);
}

void AMKIRoadJunction::ArmMarkings(int32 Lanes, float LineWidthCm, float CenterGapCm, TArray<FMKIRoadJunctionStripe>& Out)
{
	// The maintainer (#229, a North American intersection photo): each arm, from the arm's end in, carries the road's lines up to a
	// stop line across the incoming lanes, then a crosswalk of bars across the whole arm, next to the crossing square.
	// Distances along the arm are from the square's edge; lateral is to the right of the arm's outward direction.
	const float Half = Lanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f;
	const float CrosswalkFrom = CrosswalkInsetCm, CrosswalkTo = CrosswalkInsetCm + CrosswalkDepthCm, StopLine = CrosswalkTo + StopLineGapCm;
	// The crosswalk: bars along the arm, BarWidthCm wide and as far apart, across the arm inside its edge lines.
	const float Across = 2.f * (Half - CrosswalkSideInsetCm);
	const int32 Bars = FMath::Max(1, FMath::FloorToInt32((Across + BarWidthCm) / (2.f * BarWidthCm)));
	const float Used = Bars * 2.f * BarWidthCm - BarWidthCm;
	for (int32 Bar = 0; Bar < Bars; ++Bar)
	{
		const float Lateral = -Used * 0.5f + BarWidthCm * 0.5f + Bar * 2.f * BarWidthCm;
		Out.Add({ (CrosswalkFrom + CrosswalkTo) * 0.5f, Lateral, CrosswalkDepthCm, BarWidthCm, false });
	}
	// The stop line across the incoming lanes: traffic arriving runs against the arm's outward direction, so its lanes are
	// on the arm's left (negative lateral); a one-lane arm stops across its whole width.
	const float PairOffset = (CenterGapCm + LineWidthCm) * 0.5f;
	const float StopFrom = -Half + CrosswalkSideInsetCm, StopTo = Lanes >= 2 ? -(PairOffset + LineWidthCm * 0.5f) : Half - CrosswalkSideInsetCm;
	Out.Add({ StopLine, (StopFrom + StopTo) * 0.5f, StopLineWidthCm, StopTo - StopFrom, false });
	// The road's own lines from the stop line out to the arm's end: the double yellow center line, and between the lanes of
	// each direction a solid white line (lane changes end before an intersection).
	const float Run = ApronCm - StopLine - StopLineWidthCm * 0.5f, Middle = StopLine + StopLineWidthCm * 0.5f + Run * 0.5f;
	if (Lanes >= 2)
	{
		for (const float Side : { -1.f, 1.f }) Out.Add({ Middle, Side * PairOffset, Run, LineWidthCm, true });
		const float LaneWidth = 2.f * Half / Lanes;
		for (int32 Boundary = 1; Boundary < Lanes; ++Boundary) if (Boundary * 2 != Lanes) Out.Add({ Middle, -Half + Boundary * LaneWidth, Run, LineWidthCm, false });
	}
}

int32 AMKIRoadJunction::MarkingGeometry(int32 Arms, int32 Lanes, const UMKIRoadMarkingPattern* Pattern, FMKIRoadDeckGeometry (&Paint)[2], const TArray<int8>* DockModes)
{
	int32 Count = 0;
	if (!Pattern) return 0;
	const FMKIRoadJunctionLayout Shape = Layout(Arms, Lanes);
	const float Lift = Pattern->LiftCm;
	// A painted rectangle on the deck top: Center, its length along Along, and its width across.
	auto AddQuad = [Lift](FMKIRoadDeckGeometry& Out, const FVector2D& Center, const FVector2D& Along, float Length, float Width)
	{
		const FVector2D Side(-Along.Y, Along.X);
		const int32 First = Out.Vertices.Num();
		for (const float A : { -0.5f, 0.5f }) for (const float B : { -0.5f, 0.5f })
		{
			const FVector2D Point = Center + Along * (A * Length) + Side * (B * Width);
			Out.Vertices.Add(FVector(Point.X, Point.Y, Lift));
			Out.Normals.Add(FVector::UpVector);
			Out.UVs.Add(FVector2D::ZeroVector);
			Out.UV1s.Add(FVector2D::ZeroVector);
			Out.Tangents.Add(FVector(Along.X, Along.Y, 0.f));
		}
		// Two triangles, each facing up.
		for (const FIntVector& Triangle : { FIntVector(0, 1, 2), FIntVector(1, 3, 2) })
		{
			int32 I0 = First + Triangle.X, I1 = First + Triangle.Y, I2 = First + Triangle.Z;
			const FVector Face = FVector::CrossProduct(Out.Vertices[I1] - Out.Vertices[I0], Out.Vertices[I2] - Out.Vertices[I0]);
			if (FVector::DotProduct(Face, FVector::UpVector) > 0.f) Swap(I1, I2);
			Out.Triangles.Append({ I0, I1, I2 });
		}
	};
	// A painted polygon on the deck top, fanned from its first point (convex, or star-shaped from it), facing up.
	auto AddFan = [Lift](FMKIRoadDeckGeometry& Out, const TArray<FVector2D>& Polygon)
	{
		if (Polygon.Num() < 3) return false;
		const int32 First = Out.Vertices.Num();
		for (const FVector2D& Point : Polygon)
		{
			Out.Vertices.Add(FVector(Point.X, Point.Y, Lift));
			Out.Normals.Add(FVector::UpVector);
			Out.UVs.Add(FVector2D::ZeroVector);
			Out.UV1s.Add(FVector2D::ZeroVector);
			Out.Tangents.Add(FVector::ForwardVector);
		}
		for (int32 Index = 1; Index + 1 < Polygon.Num(); ++Index)
		{
			if (FMath::Abs(FVector2D::CrossProduct(Polygon[Index] - Polygon[0], Polygon[Index + 1] - Polygon[0])) < 1.0) continue;
			int32 B = First + Index, C = First + Index + 1;
			if (FVector::DotProduct(FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[First], Out.Vertices[C] - Out.Vertices[First]), FVector::UpVector) > 0.f) Swap(B, C);
			Out.Triangles.Append({ First, B, C });
		}
		return true;
	};
	// A word in Roboto Black (#318, from the font's outlines) centred on Center: Up is the letters' up, Right the reading
	// direction, ScaleAcross and ScaleUp the cap height across and up (letters stretched up as road words are).
	auto AddWord = [Lift](FMKIRoadDeckGeometry& Out, const TCHAR* Word, const FVector2D& Center, const FVector2D& Up, const FVector2D& Right, float ScaleAcross, float ScaleUp)
	{
		const FVector2D Origin = Center - Right * (LegendWidth(Word) * ScaleAcross * 0.5f) - Up * (ScaleUp * 0.5f);
		float Pen = 0.f;
		for (const TCHAR* Letter = Word; *Letter; ++Letter)
		{
			const FMKIRoadLegendGlyph* Glyph = LegendGlyph(*Letter);
			if (!Glyph) continue;
			for (int32 Vertex = Glyph->First; Vertex + 2 < Glyph->First + Glyph->Vertices; Vertex += 3)
			{
				const int32 Base = Out.Vertices.Num();
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const float X = GMKIRoadLegendGlyphPoints[(Vertex + Corner) * 2], Y = GMKIRoadLegendGlyphPoints[(Vertex + Corner) * 2 + 1];
					const FVector2D Point = Origin + Right * ((Pen + X) * ScaleAcross) + Up * (Y * ScaleUp);
					Out.Vertices.Add(FVector(Point.X, Point.Y, Lift));
					Out.Normals.Add(FVector::UpVector);
					Out.UVs.Add(FVector2D::ZeroVector);
					Out.UV1s.Add(FVector2D::ZeroVector);
					Out.Tangents.Add(FVector::ForwardVector);
				}
				int32 B = Base + 1, C = Base + 2;
				// Facing up, whichever way the reading frame turned it.
				if (FVector::DotProduct(FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[Base], Out.Vertices[C] - Out.Vertices[Base]), FVector::UpVector) > 0.f) Swap(B, C);
				Out.Triangles.Append({ Base, B, C });
			}
			Pen += Glyph->Advance;
		}
	};
	// The non-drivable margin (#316; the maintainer: a trim to the edge, not a line near it): white from every outline edge in to
	// MKIRoadEdgeTrimCm, but the arm ends, where roads join and carry their own trims; yellow round a depot's station pads
	// (#254 had no line there; the maintainer then asked for a yellow edge round the factory-zone stripes).
	const float Trim = MKIRoadEdgeTrimCm(*Pattern);
	for (int32 EdgeIndex = 0; EdgeIndex < Shape.OutlineEdges.Num(); ++EdgeIndex)
	{
		if (Shape.OutlineIsArmEnd[EdgeIndex]) continue;
		// A corner tile's edges lie inside its filled curb corner (below).
		if (Shape.CellIsCorner.IsValidIndex(Shape.OutlineCells[EdgeIndex]) && Shape.CellIsCorner[Shape.OutlineCells[EdgeIndex]]) continue;
		const bool bPad = Shape.CellIsPad.IsValidIndex(Shape.OutlineCells[EdgeIndex]) && Shape.CellIsPad[Shape.OutlineCells[EdgeIndex]];
		const FVector2D P = Shape.OutlineEdges[EdgeIndex].Key, Q = Shape.OutlineEdges[EdgeIndex].Value;
		const FVector2D Normal = -OutwardNormal(Shape.Cells[Shape.OutlineCells[EdgeIndex]], P, Q);
		const FVector2D Along = (Q - P).GetSafeNormal();
		AddQuad(Paint[bPad ? 0 : 1], (P + Q) * 0.5 + Normal * (Trim * 0.5f), Along, static_cast<float>((Q - P).Size()), Trim);
		++Count;
	}
	// Each corner tile (#230, #314, #316): white beyond the curb return, the quarter circle of CurbRadiusCm about the tile's
	// outer corner widened by the trim, so its curve meets the arms' trims where they leave the tile.
	for (int32 CellIndex = 0; CellIndex < Shape.Cells.Num(); ++CellIndex)
	{
		if (!Shape.CellIsCorner.IsValidIndex(CellIndex) || !Shape.CellIsCorner[CellIndex]) continue;
		const FBox2D Tile(Shape.Cells[CellIndex]);
		// The outer corner is the tile's corner farthest from the junction's center.
		const FVector2D Outer(FMath::Abs(Tile.Min.X) > FMath::Abs(Tile.Max.X) ? Tile.Min.X : Tile.Max.X, FMath::Abs(Tile.Min.Y) > FMath::Abs(Tile.Max.Y) ? Tile.Min.Y : Tile.Max.Y);
		const FVector2D Toward(Outer.X > 0.0 ? -1.0 : 1.0, Outer.Y > 0.0 ? -1.0 : 1.0);
		const float Radius = CurbRadiusCm + Trim;
		TArray<FVector2D> Corner = { Outer };
		for (int32 Step = 0; Step <= CornerArcSteps; ++Step)
		{
			const float Angle = HALF_PI * Step / CornerArcSteps;
			Corner.Add(Outer + FVector2D(Toward.X * FMath::Sin(Angle), Toward.Y * FMath::Cos(Angle)) * Radius);
		}
		if (AddFan(Paint[1], Corner)) ++Count;
	}
	if (Shape.bTurnaround)
	{
		// A turnaround (#316): white outside the round deck it had before #314 (the hull of its round end and its arm end),
		// drawn in by the trim, out to the rectangle's edge. Between that curve and each edge, along rays from the center.
		const float ArmEnd = TurnaroundArmEndCm(Lanes), Half = Lanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f, Radius = TurnaroundDeckRadiusCm(Lanes) - Trim;
		constexpr int32 Segments = 96;
		TArray<FVector2D> Points = { FVector2D(ArmEnd, -(Half - Trim)), FVector2D(ArmEnd, Half - Trim) };
		for (int32 Step = 0; Step < Segments; ++Step)
		{
			const FVector2D Point = FVector2D(Radius, 0.0).GetRotated(360.0 * Step / Segments);
			if (Point.X < ArmEnd - 1.0) Points.Add(Point);
		}
		const TArray<FVector2D> Hull = ConvexHull(Points);
		FBox2D Box(ForceInit);
		for (const TArray<FVector2D>& Cell : Shape.Cells) for (const FVector2D& Point : Cell) Box += Point;
		const FVector2D Corners[4] = { Box.Min, FVector2D(Box.Max.X, Box.Min.Y), Box.Max, FVector2D(Box.Min.X, Box.Max.Y) };
		auto AngleOf = [](const FVector2D& V) { return FMath::Atan2(V.Y, V.X); };
		for (int32 Index = 0; Index < Hull.Num(); ++Index)
		{
			const FVector2D A = Hull[Index], B = Hull[(Index + 1) % Hull.Num()];
			const FVector2D OutA = RayToBox(Box, A), OutB = RayToBox(Box, B);
			// The rectangle's corners the sweep from A to B passes (counter-clockwise), in order.
			TArray<FVector2D> Between;
			double Sweep = AngleOf(B) - AngleOf(A);
			if (Sweep < 0.0) Sweep += 2.0 * PI;
			for (const FVector2D& Corner : Corners)
			{
				double At = AngleOf(Corner) - AngleOf(A);
				if (At < 0.0) At += 2.0 * PI;
				if (At > 0.0 && At < Sweep) Between.Add(Corner);
			}
			Between.Sort([&](const FVector2D& L, const FVector2D& M) { double X = AngleOf(L) - AngleOf(A), Y = AngleOf(M) - AngleOf(A); if (X < 0.0) X += 2.0 * PI; if (Y < 0.0) Y += 2.0 * PI; return X < Y; });
			// A, then out along the rectangle from A's ray to B's, then B: star-shaped from A for these short steps.
			TArray<FVector2D> Piece = { A, OutA };
			Piece.Append(Between);
			Piece.Add(OutB);
			Piece.Add(B);
			if (FVector2D::DistSquared(A, OutA) < 1.0 && FVector2D::DistSquared(B, OutB) < 1.0) continue;
			if (AddFan(Paint[1], Piece)) ++Count;
		}
	}
	if (Shape.bTurnaround)
	{
		// A turnaround (#236): a yellow ring round the island inside the inner loop and, between the arriving and leaving
		// lanes, the road's double yellow center line from the arm end in to the island.
		const float Island = TurnaroundIslandRadiusCm() - Pattern->LineWidthCm * 0.5f;
		constexpr int32 RingSteps = 48;
		for (int32 Step = 0; Step < RingSteps; ++Step)
		{
			const FVector2D A = FVector2D(Island, 0.0).GetRotated(360.0 * Step / RingSteps), B = FVector2D(Island, 0.0).GetRotated(360.0 * (Step + 1) / RingSteps);
			AddQuad(Paint[0], (A + B) * 0.5, (B - A).GetSafeNormal(), static_cast<float>((B - A).Size()) + Pattern->LineWidthCm * 0.5f, Pattern->LineWidthCm);
			++Count;
		}
		if (Lanes >= 2)
		{
			const float ArmEnd = TurnaroundArmEndCm(Lanes), Inner = TurnaroundIslandRadiusCm(), PairOffset = (Pattern->CenterGapCm + Pattern->LineWidthCm) * 0.5f;
			for (const float Side : { -1.f, 1.f })
			{
				const float From = FMath::Sqrt(FMath::Max(0.f, Inner * Inner - PairOffset * PairOffset));
				AddQuad(Paint[0], FVector2D((ArmEnd + From) * 0.5f, Side * PairOffset), FVector2D(1.0, 0.0), ArmEnd - From, Pattern->LineWidthCm);
				++Count;
			}
		}
	}
	if (Shape.bTruckDepot)
	{
		// The station pads' factory-zone stripes, in the pattern's yellow under its other paint (#322: they were their own
		// mesh on the deck, outside the pattern).
		Count += PadStripeGeometry(Arms, Lanes, Paint[0]);
		// A truck depot (#237, #240): the road's lines along its length, and a dashed line beside each bay.
		const float Reach = DepotHalfLengthCm, Half = Lanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f, Width = Pattern->LineWidthCm;
		auto Dashed = [&](float Lateral, float From, float To)
		{
			const float Period = FMath::Max(1.f, Pattern->DashLengthCm + Pattern->DashGapCm);
			for (float X = From; X < To - 1.f; X += Period)
			{
				const float Length = FMath::Min(Pattern->DashLengthCm, To - X);
				AddQuad(Paint[1], FVector2D(X + Length * 0.5f, Lateral), FVector2D(1.0, 0.0), Length, Width);
				++Count;
			}
		};
		if (Lanes >= 2)
		{
			const float PairOffset = (Pattern->CenterGapCm + Width) * 0.5f;
			for (const float Side : { -1.f, 1.f }) { AddQuad(Paint[0], FVector2D(0.0, Side * PairOffset), FVector2D(1.0, 0.0), 2.f * Reach, Width); ++Count; }
			for (int32 Boundary = 1; Boundary < Lanes; ++Boundary) if (Boundary * 2 != Lanes) Dashed(-Half + Boundary * AMKIRoadSplineRoad::LaneWidthCm, -Reach, Reach);
		}
		// The bay line (#318): solid the whole length (the maintainer: with the bay striped, it need not be dashed where the bends
		// cross it), and the joined road's edge trim carried on along the depot: as wide, at the same place inside the lane's
		// edge (the maintainer: the road's trim line just extends on).
		for (const float Side : { 1.f, -1.f })
		{
			if (Side < 0.f && !IsBothSidesShape(Arms)) continue;
			AddQuad(Paint[1], FVector2D(0.0, Side * (Half - Trim * 0.5f)), FVector2D(1.0, 0.0), 2.f * Reach, Trim);
			++Count;
		}
		// At each road end, on each bay side, the corner square between the joined road's edge trim and the trim down the
		// bay's end, so the two meet at a right angle (#316). The bay's ends are plain white trim (#318: the maintainer, the quarter
		// circles of #316 replaced).
		for (const float Side : { 1.f, -1.f })
		{
			if (Side < 0.f && !IsBothSidesShape(Arms)) continue;
			for (const float End : { -1.f, 1.f }) { AddQuad(Paint[1], FVector2D(End * (Reach - Trim * 0.5f), Side * (Half - Trim * 0.5f)), FVector2D(1.0, 0.0), Trim, Trim); ++Count; }
		}
		// The pad's side facing the deck, in front of each bay: the bay's white trim and the pad's yellow one side by side.
		for (int32 CellIndex = 0; CellIndex < Shape.Cells.Num(); ++CellIndex)
		{
			if (!Shape.CellIsPad.IsValidIndex(CellIndex) || !Shape.CellIsPad[CellIndex]) continue;
			const TArray<FVector2D>& Cell = Shape.Cells[CellIndex];
			for (int32 Index = 0; Index < Cell.Num(); ++Index)
			{
				const FVector2D P = Cell[Index], Q = Cell[(Index + 1) % Cell.Num()];
				const FVector2D Outward = OutwardNormal(Cell, P, Q), Probe = (P + Q) * 0.5 + Outward * 10.0;
				bool bFacesDeck = false;
				for (int32 Other = 0; Other < Shape.Cells.Num() && !bFacesDeck; ++Other)
				{
					if (Other == CellIndex || (Shape.CellIsPad.IsValidIndex(Other) && Shape.CellIsPad[Other])) continue;
					const FBox2D Box(Shape.Cells[Other]);
					bFacesDeck = Box.IsInside(Probe);
				}
				if (!bFacesDeck) continue;
				// White on the deck side, carrying the bay's outer trim straight past the pad, and yellow on the pad side, so the bay
				// has a white edge all round and the pad a yellow one (#318, the maintainer).
				AddQuad(Paint[1], (P + Q) * 0.5 + Outward * (Trim * 0.5f), (Q - P).GetSafeNormal(), static_cast<float>((Q - P).Size()), Trim);
				AddQuad(Paint[0], (P + Q) * 0.5 - Outward * (Trim * 0.5f), (Q - P).GetSafeNormal(), static_cast<float>((Q - P).Size()), Trim);
				++Count;
				++Count;
			}
		}
		// Each dock (#318, the maintainer: the bay looked like a lane): a stall box drawn as wide as the edge trims, LOADING ONLY or
		// UNLOADING ONLY inside it as its station is set (two centred lines, their bottoms toward the station), and white
		// stripes over the rest of the bay, as the pad's.
		TArray<FMKIRoadDepotDock> Docks;
		DepotDocks(Arms, Lanes, Docks);
		const float Tile = AMKIRoadSplineRoad::LaneWidthCm;
		for (int32 DockIndex = 0; DockIndex < Docks.Num(); ++DockIndex)
		{
			const float Side = Docks[DockIndex].Point.Y >= 0.0 ? 1.f : -1.f;
			const float Inner = Half + StallInsetCm, Outer = Half + Tile - Trim - StallInsetCm, Across = (Inner + Outer) * 0.5f, Depth = Outer - Inner;
			for (const float End : { -1.f, 1.f }) { AddQuad(Paint[1], FVector2D(End * StallHalfLengthCm, Side * Across), FVector2D(0.0, 1.0), Depth + Trim, Trim); ++Count; }
			for (const float Edge : { Inner, Outer }) { AddQuad(Paint[1], FVector2D(0.0, Side * Edge), FVector2D(1.0, 0.0), 2.f * StallHalfLengthCm + Trim, Trim); ++Count; }
			// Stripes outside the box: from the bay line to the pad's yellow trim, the box's surround cut out.
			const float BoxX = StallHalfLengthCm + Trim * 0.5f, Near = Half, Far = Half + Tile - Trim;
			const TPair<FVector2D, FVector2D> Areas[4] = {
				{ FVector2D(-Reach, Near), FVector2D(-BoxX, Far) }, { FVector2D(BoxX, Near), FVector2D(Reach, Far) },
				{ FVector2D(-BoxX, Near), FVector2D(BoxX, Inner - Trim * 0.5f) }, { FVector2D(-BoxX, Outer + Trim * 0.5f), FVector2D(BoxX, Far) } };
			const FVector2D Slant = FVector2D(1.0, -1.0).GetSafeNormal();
			for (const TPair<FVector2D, FVector2D>& Area : Areas)
			{
				const double Y0 = Side > 0.f ? Area.Key.Y : -Area.Value.Y, Y1 = Side > 0.f ? Area.Value.Y : -Area.Key.Y;
				if (Area.Value.X - Area.Key.X < 1.0 || Y1 - Y0 < 1.0) continue;
				const TArray<FVector2D> Rect = { FVector2D(Area.Key.X, Y0), FVector2D(Area.Value.X, Y0), FVector2D(Area.Value.X, Y1), FVector2D(Area.Key.X, Y1) };
				double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
				for (const FVector2D& Corner : Rect) { Low = FMath::Min(Low, FVector2D::DotProduct(Corner, Slant)); High = FMath::Max(High, FVector2D::DotProduct(Corner, Slant)); }
				// Bands on the same 45 degree grid as the pad's, so they line up from one area to the next.
				for (double From = FMath::FloorToDouble(Low / (2.0 * PadStripeWidthCm)) * 2.0 * PadStripeWidthCm; From < High; From += 2.0 * PadStripeWidthCm)
				{
					if (AddFan(Paint[1], ClipHalfPlane(ClipHalfPlane(Rect, Slant, From, false), Slant, From + PadStripeWidthCm, true))) ++Count;
				}
			}
			const int8 Mode = DockModes && DockModes->IsValidIndex(DockIndex) ? (*DockModes)[DockIndex] : -1;
			if (Mode < 0) continue;
			const TCHAR* Word = Mode > 0 ? TEXT("LOADING") : TEXT("UNLOADING");
			// Roboto Black (the maintainer, #318), from the font's own outlines. One size for both modes: the longer word fills the box's
			// length, the letters stretched LegendStretch times taller than the face, within the box's depth.
			const float InsideLength = 2.f * StallHalfLengthCm - Trim - 2.f * LegendMarginCm, InsideDepth = Depth - Trim - 2.f * LegendMarginCm;
			const float ScaleAcross = InsideLength / LegendWidth(TEXT("UNLOADING")), ScaleUp = FMath::Min(InsideDepth, ScaleAcross * LegendStretch);
			// Read from the lanes, facing the station (the maintainer: it read mirrored with up turned away from the station).
			AddWord(Paint[1], Word, FVector2D(0.0, Side * Across), FVector2D(0.0, Side), FVector2D(-Side, 0.0), ScaleAcross, ScaleUp);
			++Count;
		}
	}
	// Each arm's crosswalk, stop line, and lines (#229).
	TArray<FMKIRoadJunctionStripe> Stripes;
	ArmMarkings(Lanes, Pattern->LineWidthCm, Pattern->CenterGapCm, Stripes);
	const float Half = Lanes * AMKIRoadSplineRoad::LaneWidthCm * 0.5f;
	for (const FMKIRoadJunctionArm& Arm : (Shape.bTurnaround || Shape.bTruckDepot ? TArray<FMKIRoadJunctionArm>() : Shape.Arms))
	{
		const FVector2D Out = Arm.Outward, Right(-Out.Y, Out.X);
		const FVector2D Edge = Out * Half;
		for (const FMKIRoadJunctionStripe& Stripe : Stripes)
		{
			AddQuad(Paint[Stripe.bYellow ? 0 : 1], Edge + Out * Stripe.AlongCm + Right * Stripe.LateralCm, Out, Stripe.LengthCm, Stripe.WidthCm);
			++Count;
		}
		// STOP on each arriving lane just before its stop line (#318, the maintainer), read by the driver arriving: the letters' up
		// toward the crossing, their bottoms toward the arm's end. Arriving lanes are on the arm's left (negative lateral).
		const float StopFrom = CrosswalkInsetCm + CrosswalkDepthCm + StopLineGapCm + StopLineWidthCm * 0.5f + StopWordGapCm;
		const float StopHeight = FMath::Min(StopWordHeightCm, ApronCm - StopFrom - StopWordGapCm);
		const float LaneWidth = AMKIRoadSplineRoad::LaneWidthCm;
		for (int32 Lane = 0; Lane < FMath::Max(1, Lanes / 2) && StopHeight > 0.f; ++Lane)
		{
			const FVector2D Center = Edge + Out * (StopFrom + StopHeight * 0.5f) + Right * (-Half + (Lane + 0.5f) * LaneWidth);
			AddWord(Paint[1], TEXT("STOP"), Center, -Out, -Right, StopHeight / LegendStretch, StopHeight);
			++Count;
		}
	}
	return Count;
}

int32 AMKIRoadJunction::PadStripeGeometry(int32 Arms, int32 Lanes, FMKIRoadDeckGeometry& Out)
{
	if (!IsDepotShape(Arms)) return 0;
	TArray<FMKIRoadDepotDock> Docks;
	DepotDocks(Arms, Lanes, Docks);
	const FVector2D Across = FVector2D(1.0, -1.0).GetSafeNormal();
	int32 Count = 0;
	for (const FMKIRoadDepotDock& Dock : Docks)
	{
		const FBox2D Pad(DepotPadCorners(Dock));
		const TArray<FVector2D> Rect = { Pad.Min, FVector2D(Pad.Max.X, Pad.Min.Y), Pad.Max, FVector2D(Pad.Min.X, Pad.Max.Y) };
		// Bands at 45 degrees, PadStripeWidthCm wide every twice that, as the Full Factory Zone pattern's.
		double Low = TNumericLimits<double>::Max(), High = TNumericLimits<double>::Lowest();
		for (const FVector2D& Corner : Rect) { Low = FMath::Min(Low, FVector2D::DotProduct(Corner, Across)); High = FMath::Max(High, FVector2D::DotProduct(Corner, Across)); }
		for (double From = FMath::FloorToDouble(Low / (2.0 * PadStripeWidthCm)) * 2.0 * PadStripeWidthCm; From < High; From += 2.0 * PadStripeWidthCm)
		{
			const TArray<FVector2D> Band = ClipHalfPlane(ClipHalfPlane(Rect, Across, From, false), Across, From + PadStripeWidthCm, true);
			if (Band.Num() < 3) continue;
			const int32 First = Out.Vertices.Num();
			for (const FVector2D& Point : Band)
			{
				// Below the markings (LiftCm, 0.5) so the yellow pad trim (#316) draws over the stripes.
				Out.Vertices.Add(FVector(Point.X, Point.Y, 0.3f));
				Out.Normals.Add(FVector::UpVector);
				Out.UVs.Add(FVector2D::ZeroVector);
				Out.UV1s.Add(FVector2D::ZeroVector);
				Out.Tangents.Add(FVector::ForwardVector);
			}
			for (int32 Index = 1; Index + 1 < Band.Num(); ++Index)
			{
				int32 B = First + Index, C = First + Index + 1;
				// Facing up (a procedural mesh face shows opposite (B - A) x (C - A)).
				if (FVector::DotProduct(FVector::CrossProduct(Out.Vertices[B] - Out.Vertices[First], Out.Vertices[C] - Out.Vertices[First]), FVector::UpVector) > 0.f) Swap(B, C);
				Out.Triangles.Append({ First, B, C });
			}
			++Count;
		}
	}
	return Count;
}

void AMKIRoadJunction::BuildPadStripes()
{
	// The pad stripes are part of the road pattern's paint now (#322, the maintainer: no marking painted onto the deck itself);
	// this mesh stays empty, kept so older saves load as they were.
	if (mPadStripes) mPadStripes->ClearAllMeshSections();
}

TArray<int8> AMKIRoadJunction::GetDockModes() const
{
	TArray<int8> Modes;
	if (!IsTruckDepot() || !GetWorld()) return Modes;
	for (const FTransform& Dock : GetDepotDocks())
	{
		int8 Mode = -1;
		for (TActorIterator<AFGBuildableDockingStation> It(GetWorld()); It && Mode < 0; ++It)
		{
			if (!IsValid(*It) || It->IsActorBeingDestroyed() || FVector::Dist(It->GetActorLocation(), Dock.GetLocation()) > 2.f * FMKIRoadAutoPaths::JoinRadiusCm) continue;
			Mode = It->GetIsInLoadMode() ? 1 : 0;
		}
		Modes.Add(Mode);
	}
	return Modes;
}

void AMKIRoadJunction::PollDockModes()
{
	// A station already standing on a dock with legs reaching under the deck (#321: placed before the dock snap cleared them)
	// has them cleared; the game rebuilds its legs from these offsets when the save next loads.
	if (IsTruckDepot() && HasAuthority() && GetWorld())
	{
		for (const FTransform& Dock : GetDepotDocks())
		{
			for (TActorIterator<AFGBuildableDockingStation> It(GetWorld()); It; ++It)
			{
				if (!IsValid(*It) || FVector::Dist(It->GetActorLocation(), Dock.GetLocation()) > 2.f * FMKIRoadAutoPaths::JoinRadiusCm) continue;
				UFGFactoryLegsComponent* Legs = It->FindComponentByClass<UFGFactoryLegsComponent>();
				if (!Legs) continue;
				bool bLegs = false;
				for (const FFeetOffset& Foot : Legs->GetCachedFeetOffsets()) bLegs |= FMath::Abs(Foot.OffsetZ) > 1.f;
				if (bLegs) Legs->SetFeetOffsetsToZero();
			}
		}
	}
	if (mPaintedPattern && GetDockModes() != mPaintedDockModes) BuildMarkings(mPaintedPattern);
}

void AMKIRoadJunction::BuildMarkings(const UMKIRoadMarkingPattern* Pattern)
{
	mMarkingLineCount = 0;
	if (!mMarkings) return;
	mMarkings->ClearAllMeshSections();
	mPaintedPattern = Pattern;
	if (!Pattern) return;
	FMKIRoadDeckGeometry Paint[2];
	mPaintedDockModes = GetDockModes();
	// A depot repaints its LOAD or UNLOAD legend when a station's mode changes (#318); the station has no event for it.
	if (IsTruckDepot() && GetWorld() && !GetWorld()->GetTimerManager().IsTimerActive(mDockModeTimer))
		GetWorld()->GetTimerManager().SetTimer(mDockModeTimer, FTimerDelegate::CreateWeakLambda(this, [this]() { PollDockModes(); }), DockModePollSeconds, true);
	mMarkingLineCount = MarkingGeometry(mArms, mLanes, Pattern, Paint, &mPaintedDockModes);
	const TSoftObjectPtr<UMaterialInterface> Materials[2] = { Pattern->YellowMaterial, Pattern->WhiteMaterial };
	for (int32 Section = 0; Section < 2; ++Section)
	{
		TArray<FProcMeshTangent> Tangents;
		for (const FVector& Tangent : Paint[Section].Tangents) Tangents.Add(FProcMeshTangent(Tangent, false));
		mMarkings->CreateMeshSection_LinearColor(Section, Paint[Section].Vertices, Paint[Section].Triangles, Paint[Section].Normals, Paint[Section].UVs, Paint[Section].UV1s, TArray<FVector2D>(), TArray<FVector2D>(), TArray<FLinearColor>(), Tangents, false);
		if (UMaterialInterface* Material = Materials[Section].LoadSynchronous()) mMarkings->SetMaterial(Section, Material);
	}
}

TArray<FTransform> AMKIRoadJunction::GetArmEnds() const
{
	TArray<FTransform> Ends;
	const FTransform Frame = GetActorTransform();
	for (const FMKIRoadJunctionArm& Arm : Layout(mArms, mLanes).Arms)
	{
		const FVector Outward = Frame.TransformVectorNoScale(FVector(Arm.Outward.X, Arm.Outward.Y, 0.f));
		Ends.Add(FTransform(Outward.Rotation(), Frame.TransformPosition(FVector(Arm.Center.X, Arm.Center.Y, 0.f))));
	}
	return Ends;
}

bool AMKIRoadJunction::NearSide(const FVector& Aim, FVector& OutPoint, FVector& OutOutward, bool& bOutArm) const
{
	const FMKIRoadJunctionLayout Shape = Layout(mArms, mLanes);
	FBox2D Bounds(ForceInit);
	for (const TArray<FVector2D>& Cell : Shape.Cells) for (const FVector2D& Corner : Cell) Bounds += Corner;
	if (!Bounds.bIsValid) return false;
	const FTransform Frame = GetActorTransform();
	const FVector Local = Frame.InverseTransformPosition(Aim);
	const double Band = AMKIRoadSplineRoad::SnapBandCm;
	// Each side: its outward direction, how far in from it the aim is, and the band.
	const struct { FVector2D Side; double In; double Band; } Sides[4] = {
		{ FVector2D(1.0, 0.0), Bounds.Max.X - Local.X, Band }, { FVector2D(-1.0, 0.0), Local.X - Bounds.Min.X, Band },
		{ FVector2D(0.0, 1.0), Bounds.Max.Y - Local.Y, Band }, { FVector2D(0.0, -1.0), Local.Y - Bounds.Min.Y, Band } };
	int32 Best = INDEX_NONE;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		if (Sides[Index].In > Sides[Index].Band) continue;
		if (Best == INDEX_NONE || Sides[Index].In < Sides[Best].In) Best = Index;
	}
	if (Best == INDEX_NONE) return false;
	const FVector2D Side = Sides[Best].Side;
	auto OnGrid = [](double Value, double Min, double Max) { return FMath::Clamp(FMath::GridSnap(Value, 100.0), Min, Max); };
	FVector2D Point = Side.X != 0.0 ? FVector2D(Side.X > 0.0 ? Bounds.Max.X : Bounds.Min.X, OnGrid(Local.Y, Bounds.Min.Y, Bounds.Max.Y))
		: FVector2D(OnGrid(Local.X, Bounds.Min.X, Bounds.Max.X), Side.Y > 0.0 ? Bounds.Max.Y : Bounds.Min.Y);
	bOutArm = false;
	double Nearest = TNumericLimits<double>::Max();
	for (const FMKIRoadJunctionArm& Arm : Shape.Arms)
	{
		if (FVector2D::DotProduct(Arm.Outward.GetSafeNormal(), Side) <= 0.99) continue;
		const double Distance = FVector2D::Distance(Arm.Center, FVector2D(Local.X, Local.Y));
		if (Distance >= Nearest) continue;
		Nearest = Distance;
		Point = Arm.Center;
		bOutArm = true;
	}
	OutPoint = Frame.TransformPosition(FVector(Point.X, Point.Y, 0.f));
	OutOutward = Frame.TransformVectorNoScale(FVector(Side.X, Side.Y, 0.f)).GetSafeNormal2D();
	return true;
}

int32 AMKIRoadJunction::GetDismantleRefundReturnsMultiplier() const
{
	return FoundationEquivalents(mArms, mLanes);
}

FBox AMKIRoadJunction::CalculateBounds() const
{
	const FBox2D Box = LayoutBounds(Layout(mArms, mLanes));
	return FBox(FVector(Box.Min.X, Box.Min.Y, -mDeckThicknessCm), FVector(Box.Max.X, Box.Max.Y, 0.f)).TransformBy(GetActorTransform());
}
