#include "MKIRoadPaintGuard.h"

#include "MKIRoadJunction.h"
#include "MKIRoadTile.h"
#include "MKIRoadSplineRoad.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "FGConstructDisqualifier.h"
#include "FGCustomizationRecipe.h"
#include "FGFactoryColoringTypes.h"

namespace
{
	// The paint state keeps its can-apply flags private and unreflected; an explicit instantiation may name a private
	// member (C++ [temp.explicit]), which is how these are reached.
	template <typename Tag, typename Tag::Type Member>
	struct TPaintFlagAccess { friend typename Tag::Type Get(Tag) { return Member; } };
	struct FPatternFlag { using Type = bool UFGBuildGunStatePaint::*; friend Type Get(FPatternFlag); };
	struct FMaterialFlag { using Type = bool UFGBuildGunStatePaint::*; friend Type Get(FMaterialFlag); };
	struct FSwatchFlag { using Type = bool UFGBuildGunStatePaint::*; friend Type Get(FSwatchFlag); };
	struct FDisqualifiers { using Type = TArray<TSubclassOf<UFGConstructDisqualifier>> UFGBuildGunStatePaint::*; friend Type Get(FDisqualifiers); };
	template struct TPaintFlagAccess<FPatternFlag, &UFGBuildGunStatePaint::mHitCanApplyPattern>;
	template struct TPaintFlagAccess<FMaterialFlag, &UFGBuildGunStatePaint::mHitCanApplyMaterial>;
	template struct TPaintFlagAccess<FSwatchFlag, &UFGBuildGunStatePaint::mHitCanApplySwatch>;
	template struct TPaintFlagAccess<FDisqualifiers, &UFGBuildGunStatePaint::mConstructDisqualifiers>;
}

bool FMKIRoadPaintGuard::IsRefusedOnRoad(TSubclassOf<UFGFactoryCustomizationDescriptor> Customization)
{
	if (!Customization) return false;
	if (Customization->IsChildOf(UFGFactoryCustomizationDescriptor_Material::StaticClass())) return true;
	// The maintainer: vanilla colours and paint finishes (swatches) are refused too, as materials are (#225).
	if (Customization->IsChildOf(UFGFactoryCustomizationDescriptor_Swatch::StaticClass())) return true;
	return Customization->IsChildOf(UFGFactoryCustomizationDescriptor_Pattern::StaticClass())
		&& AMKIRoadSplineRoad::IsVanillaPattern(TSubclassOf<UFGFactoryCustomizationDescriptor_Pattern>(*Customization));
}

bool FMKIRoadPaintGuard::Refuses(UFGBuildGunStatePaint* Paint)
{
	AFGBuildGun* BuildGun = Paint ? Paint->GetBuildGun() : nullptr;
	if (!BuildGun) return false;
	const FHitResult& Hit = BuildGun->GetHitResult();
	AActor* HitActor = Hit.HitObjectHandle.FetchActor();
	if (!HitActor) HitActor = Hit.GetActor();
	if (!Cast<AMKIRoadSplineRoad>(HitActor) && !AMKIRoadJunction::FromActor(HitActor)) return false;
	TSubclassOf<UFGCustomizationRecipe> Recipe = Paint->GetActiveRecipe();
	UFGCustomizationRecipe* Defaults = Recipe ? Recipe->GetDefaultObject<UFGCustomizationRecipe>() : nullptr;
	return Defaults && IsRefusedOnRoad(Defaults->GetCustomizationDesc());
}

void FMKIRoadPaintGuard::PrepareAim(UFGBuildGunStatePaint* Paint)
{
	// The maintainer (#225): clearing the can-apply flags after the vanilla tick blocked the click, but the road stayed outlined as
	// a valid target. Before the tick, the aimed road's own coloring and patterning flags say what it takes, so the vanilla
	// checks refuse a swatch or vanilla pattern as they refuse a material. The last road changed is restored when the aim
	// leaves it.
	static TWeakObjectPtr<AActor> Last;
	// A road or a junction (#227) takes the same customizations.
	auto Allow = [](AActor* Target, bool bColoring, bool bPatterning)
	{
		if (AMKIRoadSplineRoad* Road = Cast<AMKIRoadSplineRoad>(Target)) Road->SetCustomizerAllows(bColoring, bPatterning);
		else if (AMKIRoadJunction* Junction = Cast<AMKIRoadJunction>(Target)) Junction->SetCustomizerAllows(bColoring, bPatterning);
		// A piece's square takes what its piece takes (paint aimed at it goes to the whole piece).
		else if (AMKIRoadTile* Tile = Cast<AMKIRoadTile>(Target)) Tile->SetCustomizerAllows(bColoring, bPatterning);
	};
	AFGBuildGun* BuildGun = Paint ? Paint->GetBuildGun() : nullptr;
	AActor* HitActor = BuildGun ? BuildGun->GetHitResult().HitObjectHandle.FetchActor() : nullptr;
	if (!HitActor && BuildGun) HitActor = BuildGun->GetHitResult().GetActor();
	AActor* Road = Cast<AMKIRoadSplineRoad>(HitActor) || AMKIRoadJunction::FromActor(HitActor) ? HitActor : nullptr;
	if (Last.IsValid() && Last.Get() != Road) Allow(Last.Get(), true, true);
	Last = Road;
	if (!Road) return;
	TSubclassOf<UFGCustomizationRecipe> Recipe = Paint->GetActiveRecipe();
	UFGCustomizationRecipe* Defaults = Recipe ? Recipe->GetDefaultObject<UFGCustomizationRecipe>() : nullptr;
	TSubclassOf<UFGFactoryCustomizationDescriptor> Customization = Defaults ? Defaults->GetCustomizationDesc() : nullptr;
	const bool bSwatch = Customization && Customization->IsChildOf(UFGFactoryCustomizationDescriptor_Swatch::StaticClass());
	const bool bVanillaPattern = Customization && Customization->IsChildOf(UFGFactoryCustomizationDescriptor_Pattern::StaticClass()) && IsRefusedOnRoad(Customization);
	Allow(Road, !bSwatch, !bSwatch && !bVanillaPattern);
}

void FMKIRoadPaintGuard::MarkInvalid(UFGBuildGunStatePaint* Paint)
{
	if (!Paint) return;
	Paint->*Get(FPatternFlag()) = false;
	Paint->*Get(FMaterialFlag()) = false;
	Paint->*Get(FSwatchFlag()) = false;
	(Paint->*Get(FDisqualifiers())).AddUnique(UFGCDInvalidCustomizationTarget::StaticClass());
}
