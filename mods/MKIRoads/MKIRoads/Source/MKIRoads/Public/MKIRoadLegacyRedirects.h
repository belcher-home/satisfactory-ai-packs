#pragma once

#include "CoreMinimal.h"

/** Redirects from the mod's pre-rename AI RoadPack names, so saves made before the rename keep their roads. */
struct MKIROADS_API FMKIRoadLegacyRedirects
{
	/** Adds the redirects to FCoreRedirects; call before any save loads (the module's StartupModule). */
	static void Register();
};
