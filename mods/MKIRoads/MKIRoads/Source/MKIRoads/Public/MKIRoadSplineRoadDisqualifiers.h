#pragma once

#include "CoreMinimal.h"
#include "FGConstructDisqualifier.h"
#include "MKIRoadSplineRoadDisqualifiers.generated.h"

// The spline road hologram's own construct disqualifiers, worded for roads: the vanilla track classes it used first
// told players that a "railroad track" turned too sharply.

UCLASS()
class MKIROADS_API UMKIRoadCDTooShort : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UMKIRoadCDTooShort() { mDisqfualifyingText = NSLOCTEXT("MKIRoads", "RoadTooShort", "Road is too short (min 4 m)"); }
};

UCLASS()
class MKIROADS_API UMKIRoadCDTooLong : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UMKIRoadCDTooLong() { mDisqfualifyingText = NSLOCTEXT("MKIRoads", "RoadTooLong", "Road is too long (max 100 m)"); }
};

UCLASS()
class MKIROADS_API UMKIRoadCDTooSteep : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UMKIRoadCDTooSteep() { mDisqfualifyingText = NSLOCTEXT("MKIRoads", "RoadTooSteep", "Road is too steep (max 25 degrees)"); }
};

UCLASS()
class MKIROADS_API UMKIRoadCDTurnTooSharp : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UMKIRoadCDTurnTooSharp() { mDisqfualifyingText = NSLOCTEXT("MKIRoads", "RoadTurnTooSharp", "Road turns too sharply for its width"); }
};

// Pieces that must be aimed at a road (the road supports, #194).

UCLASS()
class MKIROADS_API UMKIRoadCDNeedsRoad : public UFGConstructDisqualifier
{
	GENERATED_BODY()
public:
	UMKIRoadCDNeedsRoad() { mDisqfualifyingText = NSLOCTEXT("MKIRoads", "NeedsRoad", "Aim at a road"); }
};
