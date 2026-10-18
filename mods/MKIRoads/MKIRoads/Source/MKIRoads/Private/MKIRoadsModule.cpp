#include "MKIRoadBuildSnap.h"
#include "MKIRoadLegacyRedirects.h"
#include "MKIRoadCustomizerIcons.h"
#include "MKIRoadLanePath.h"
#include "MKIRoadPaintGuard.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunPaint.h"
#include "Hologram/FGDockingStationHologram.h"
#include "Hologram/FGBuildableHologram.h"
#include "Hologram/FGFactoryHologram.h"
#include "Hologram/FGFenceHologram.h"
#include "Hologram/FGWallHologram.h"
#include "Hologram/FGFoundationHologram.h"
#include "Hologram/FGVehiclePathSegmentHologram.h"
#include "Hologram/FGWalkwayHologram.h"
#include "Modules/ModuleManager.h"
#if !WITH_EDITOR
#include "Patching/NativeHookManager.h"
#endif


/**
 * MKI Roads' runtime module. In game builds it lets vanilla foundations, walkways, and catwalks snap to spline roads
 * (#188): on the deck, along an edge, or past an end, in 2 m steps (FMKIRoadBuildSnap). Every other hologram, and every
 * aim at something other than a spline road, keeps the vanilla snap. A truck station snaps to a road's edge (#201), and a street light to its edges or centerline (#222).
 * Vanilla vehicle paths on a road follow its lanes (#192, FMKIRoadLanePath). The customizer keeps vanilla materials and
 * patterns off roads (#208, FMKIRoadPaintGuard), and draws MKI Patterns' icons (#220, FMKIRoadCustomizerIcons).
 */
class FMKIRoadsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Before any save loads: map the pre-rename AI RoadPack names onto this mod's, so older saves keep their roads.
		FMKIRoadLegacyRedirects::Register();
#if !WITH_EDITOR
		// The customizer tile reads MKI Patterns as a pattern category (#220).
		FMKIRoadCustomizerIcons::Install();
		// SML hooks are game-only (as SML's own). The vanilla TrySnapToActor first offers a spline road hit to
		// FMKIRoadBuildSnap; when it places the piece, the vanilla snap is skipped.
		SUBSCRIBE_UOBJECT_METHOD(AFGFoundationHologram, TrySnapToActor, [](auto& Scope, AFGFoundationHologram* Self, const FHitResult& HitResult)
		{
			if (FMKIRoadBuildSnap::SnapHologram(Self, HitResult)) Scope.Override(true);
		});
		SUBSCRIBE_UOBJECT_METHOD(AFGWalkwayHologram, TrySnapToActor, [](auto& Scope, AFGWalkwayHologram* Self, const FHitResult& HitResult)
		{
			if (FMKIRoadBuildSnap::SnapHologram(Self, HitResult)) Scope.Override(true);
		});
		// A truck station aimed at a road (not at a vehicle path on it) goes on its edge, the bay on the edge lane (#201).
		SUBSCRIBE_UOBJECT_METHOD(AFGDockingStationHologram, TrySnapToActor, [](auto& Scope, AFGDockingStationHologram* Self, const FHitResult& HitResult)
		{
			if (FMKIRoadBuildSnap::SnapDockingStationHologram(Self, HitResult)) Scope.Override(true);
		});
		// After the vanilla factory hologram placed itself: a street light aimed at a road goes on its left edge, centerline,
		// or right edge (#222). Every other factory hologram, and every other aim, keeps the vanilla placement.
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGFactoryHologram, SetHologramLocationAndRotation, [](AFGFactoryHologram* Self, const FHitResult& HitResult)
		{
			FMKIRoadBuildSnap::SnapStreetLightHologram(Self, HitResult);
		});
		// Vanilla barriers, fences, and railings aimed at a road snap to its edges or centerline, their ends on its marks, and
		// zoop along it piece by piece, following its curves and grades (#223). Holo_Barrier is an AFGFenceHologram and
		// Holo_Railing an AFGWallHologram; any other wall, and any other aim, keeps vanilla.
		SUBSCRIBE_UOBJECT_METHOD(AFGWallHologram, TrySnapToActor, [](auto& Scope, AFGWallHologram* Self, const FHitResult& HitResult)
		{
			if (FMKIRoadBuildSnap::SnapRoadWallHologram(Self, HitResult)) Scope.Override(true);
		});
		SUBSCRIBE_UOBJECT_METHOD(AFGFenceHologram, TrySnapToActor, [](auto& Scope, AFGFenceHologram* Self, const FHitResult& HitResult)
		{
			if (FMKIRoadBuildSnap::SnapRoadWallHologram(Self, HitResult)) Scope.Override(true);
		});
		// After vanilla's zoop from the aim (the maintainer: railings did not zoop when it was skipped), the road sets the count.
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGWallHologram, SetZoopFromHitresult, [](AFGWallHologram* Self, const FHitResult& HitResult)
		{
			FIntVector Zoop;
			if (FMKIRoadBuildSnap::RoadRunZoopFromHit(Self, HitResult, Zoop)) Self->SetZoopAmount(Zoop);
		});
		// Walkways and catwalks along a road's edge zoop along it the same way (#224).
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGWalkwayHologram, SetZoopFromHitresult, [](AFGWalkwayHologram* Self, const FHitResult& HitResult)
		{
			FIntVector Zoop;
			if (FMKIRoadBuildSnap::RoadRunZoopFromHit(Self, HitResult, Zoop)) Self->SetZoopAmount(Zoop);
		});
		// The vanilla wall zoop lays instance k at k pieces along its local Y; on a road, instance k goes on the road's k-th
		// stretch instead (CreateZoopInstances is protected, so the public, non-virtual GenerateZoopInstance is hooked).
		SUBSCRIBE_METHOD(AFGBuildableHologram::GenerateZoopInstance, [](auto& Scope, AFGBuildableHologram* Self, const FTransform& InstanceTransform, bool bIsChildZoopInstance)
		{
			FTransform OnRoad;
			if (bIsChildZoopInstance || !FMKIRoadBuildSnap::RoadRunZoopInstance(Self, InstanceTransform, OnRoad)) return;
			Scope(Self, OnRoad, bIsChildZoopInstance);
		});
		// The customizer aimed at a road with a vanilla material or pattern: shown invalid each tick, and a click does nothing.
		SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStatePaint, TickState_Implementation, [](auto& Scope, UFGBuildGunStatePaint* Self, float DeltaTime)
		{
			FMKIRoadPaintGuard::PrepareAim(Self);
		});
		SUBSCRIBE_UOBJECT_METHOD_AFTER(UFGBuildGunStatePaint, TickState_Implementation, [](UFGBuildGunStatePaint* Self, float DeltaTime)
		{
			if (FMKIRoadPaintGuard::Refuses(Self)) FMKIRoadPaintGuard::MarkInvalid(Self);
		});
		SUBSCRIBE_UOBJECT_METHOD(UFGBuildGunStatePaint, PrimaryFire_Implementation, [](auto& Scope, UFGBuildGunStatePaint* Self)
		{
			if (FMKIRoadPaintGuard::Refuses(Self)) Scope.Cancel();
		});
		// After the vanilla path hologram placed itself: a start on a road moves to the center of its lane, and an end on
		// the same road plans the lane line between them.
		SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGVehiclePathSegmentHologram, SetHologramLocationAndRotation, [](AFGVehiclePathSegmentHologram* Self, const FHitResult& HitResult)
		{
			FMKIRoadLanePath::AfterSetHologramLocation(Self, HitResult);
		});
#endif
	}
};

IMPLEMENT_MODULE(FMKIRoadsModule, MKIRoads);
