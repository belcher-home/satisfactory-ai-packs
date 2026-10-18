#include "MKIRoadCustomizerIcons.h"

#include "Resources/FGItemDescriptor.h"
#include "UObject/Class.h"
#include "UObject/Stack.h"

namespace
{
	FNativeFuncPtr GOriginalGetCategory = nullptr;
	const TCHAR* const VanillaPatternCategory = TEXT("/Game/FactoryGame/Buildable/-Shared/Customization/CC_Pattern.CC_Pattern_C");

	void GetCategoryForCustomizer(UObject* Context, FFrame& Stack, RESULT_DECL)
	{
		GOriginalGetCategory(Context, Stack, RESULT_PARAM);
		// The caller's frame: the tile widget running its script.
		if (!RESULT_PARAM || !FMKIRoadCustomizerIcons::IsCustomizerTile(Stack.Object)) return;
		TSubclassOf<UObject>& Result = *static_cast<TSubclassOf<UObject>*>(RESULT_PARAM);
		Result = FMKIRoadCustomizerIcons::CategoryForTile(Result.Get());
	}
}

bool FMKIRoadCustomizerIcons::IsCustomizerTile(const UObject* ScriptObject)
{
	return ScriptObject && ScriptObject->GetClass()->GetName().StartsWith(TEXT("BPW_CustomiserIcon"));
}

UClass* FMKIRoadCustomizerIcons::CategoryForTile(UClass* Category)
{
	static UClass* Pattern = LoadClass<UObject>(nullptr, VanillaPatternCategory);
	return Pattern && Category && Category != Pattern && Category->IsChildOf(Pattern) ? Pattern : Category;
}

bool FMKIRoadCustomizerIcons::Install()
{
	if (GOriginalGetCategory) return true;
	UFunction* Function = UFGItemDescriptor::StaticClass()->FindFunctionByName(TEXT("GetCategory"));
	if (!Function || !Function->GetNativeFunc()) return false;
	GOriginalGetCategory = Function->GetNativeFunc();
	Function->SetNativeFunc(&GetCategoryForCustomizer);
	return true;
}
