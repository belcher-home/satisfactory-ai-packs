#pragma once

#include "CoreMinimal.h"

class UFGBuildGunState;
class UFGBuildGunStateDismantle;

/**
 * The dismantle gun in Terrain mode (UMKITerrainDismantleMode), driven from hooks on the vanilla dismantle state. Each tick
 * it traces from the player's view and outlines the removable rock aimed at (FMKITerrainRules); holding fire on it (as the vanilla hold to dismantle) removes it, directly on the host, or through
 * UMKIToolsRemoteCallObject from a client. Vanilla targeting is cleared and skipped while the mode is active, so nothing
 * else is outlined or dismantled; in the Default and Blueprint modes nothing changes. A friend of the vanilla state
 * (Config/AccessTransformers.ini) so it can clear the vanilla aim.
 */
class FMKITerrainDismantle
{
public:
	static bool IsTerrainMode(const UFGBuildGunStateDismantle* State);
	/** Replaces the vanilla tick in Terrain mode: the aim, its outline, and holding fire on it to remove it. */
	static void Tick(UFGBuildGunStateDismantle* State, float DeltaTime);
	/** Replaces the vanilla primary fire in Terrain mode: removes the outlined rock at once. */
	static void PrimaryFire(UFGBuildGunStateDismantle* State);
	/** How far the Terrain hold has run (0-1), for the HUD's progress bar; negative when no Terrain hold runs on this state. */
	static float HoldProgress(const UFGBuildGunState* State);
	/** Drops the aim and its outline (leaving the mode or the dismantle state). */
	static void Clear(UFGBuildGunStateDismantle* State);
};
